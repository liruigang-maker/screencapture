#include <QDateTime>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include "screencapture.h"

#define TIMEMS      qPrintable(QTime::currentTime().toString("HH:mm:ss zzz"))

ScreenCapture::ScreenCapture(QWidget* parent, QString savefile) : QThread()
{
	stop = false;
	fileName = savefile;
	frameCount = 0;
}

bool ScreenCapture::initmodules()
{
	/* 各个模块的初始化顺序不能变 */
	if (!intiDeContext())     return false;
	if (!initDecoder())       return false;
	if (!initEncoder())       return false;
	if (!initOutputStream())  return false;
	if (!initSwsContext())    return false;

	memoryAlloc();
	return true;
}

void ScreenCapture::run()
{
	int err = 0;
	if (!initmodules())
	{
		return;
	}
	qDebug() << TIMEMS << "start capture screen ...";
	while (!stop)
	{
		if ((av_read_frame(ifmt_ctx, avDePacket)) >= 0)
		{
			err = 0;
			int index = avDePacket->stream_index;
			in_stream = ifmt_ctx->streams[index];

			if (index == videoStreamIndex) 
			{
				int frameFinish = 0;
				avcodec_decode_video2(deCodecCtx, avFrameRgb, &frameFinish, avDePacket);
				sws_scale(swsContextRgbtoYuv, (const uint8_t* const*)avFrameRgb->data, avFrameRgb->linesize, \
					0, videoHeight, avFrameYuv->data, avFrameYuv->linesize);

				int base = 90000 / videoFps;
				avFrameYuv->pts = frameCount++;
				frameCount = frameCount + base;
				/* 这里设定每隔一秒钟打印一次帧号 */
				if (frameCount / base % videoFps == 0)
				{
					qDebug() << "framCount = " << frameCount / base;
				}

				int ret = avcodec_send_frame(enCodecCtx, avFrameYuv);
				if (ret != 0)
				{
					qDebug() << "avcodec_send_frame failed.";
					continue;
				}
				ret = avcodec_receive_packet(enCodecCtx, avEnPacket);
				if (ret != 0)
				{
					qDebug() << "avcodec_receive_packet failed.";
					continue;
				}
				ret = av_write_frame(ofmt_ctx, avEnPacket);
				if (ret < 0)
				{
					av_packet_unref(avEnPacket);
					av_freep(avEnPacket);
					qDebug() << "av_write_frame failed.";
					continue;
				}
			}
			av_packet_unref(avDePacket);
			av_freep(avDePacket);
		}
		else
		{
			qDebug() << "av_read_frame failed";
			av_packet_unref(avDePacket);
			av_freep(avDePacket);
			err++;
			/* 连续N次读取失败则跳出循环 */
			if (err > 3)
			{
				err = 0;
				break;
			}
		}
	}
	av_write_trailer(ofmt_ctx);
	/* close output */
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
	{
		avio_close(ofmt_ctx->pb);
	}
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ofmt_ctx);
	avformat_free_context(ifmt_ctx);

	qDebug() << TIMEMS << "stop ffmpeg thread";
}

bool ScreenCapture::intiDeContext()
{
	av_register_all();
	/* 注册编解码器 */
	avcodec_register_all();
	avdevice_register_all();
	AVDictionary* options = NULL;

	/* 指定录屏的分辨率 */
	av_dict_set(&options, "video_size", "1920x1080", 0);
	av_dict_set(&options, "pix_fmt", "yuv420p", 0);

	ifmt_ctx = avformat_alloc_context();
	if (!ifmt_ctx)
	{
		qDebug() << "avformat_alloc_context failed.";
		return false;
	}
	AVInputFormat* ifmt = av_find_input_format("gdigrab");
	if (avformat_open_input(&ifmt_ctx, "desktop", ifmt, &options) != 0) {
		qDebug() << TIMEMS << "open input error" << url;
		return false;
	}

	/* 释放设置参数 */
	if (options != NULL) {
		av_dict_free(&options);
	}
	/* 获取流信息 */
	bool result = avformat_find_stream_info(ifmt_ctx, NULL);
	if (result < 0) {
		qDebug() << TIMEMS << "find stream info error";
		return false;
	}

	AVCodec* videoDecoder = NULL;
	videoStreamIndex = -1;

	videoStreamIndex = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &videoDecoder, 0);
	if (videoStreamIndex < 0) {
		qDebug() << TIMEMS << "find video stream index error";
		return false;
	}
	/* 获取输入视频流 */
	in_stream = ifmt_ctx->streams[videoStreamIndex];
	if (!in_stream)
	{
		qDebug() << "Failed get input stream.";
		return false;
	}

	/* 获取分辨率大小 */
	videoWidth = in_stream->codec->width;
	videoHeight = in_stream->codec->height;

	/* 如果没有获取到宽高则返回 */
	if (videoWidth == 0 || videoHeight == 0) {
		qDebug() << TIMEMS << "find width height error";
		avformat_free_context(ofmt_ctx);
		avformat_free_context(ifmt_ctx);
		return false;
	}

	/* 获取视频流的帧率 fps,要对0进行过滤 */
	int num = in_stream->codec->framerate.num;
	int den = in_stream->codec->framerate.den;
	if (num != 0 && den != 0) {
		videoFps = num / den;
	}

	QString videoInfo = QString("保存视频流信息 -> 索引: %1  解码: %2  格式: %3  时长: %4 秒  fps: %5  分辨率: %6*%7  rtsp地址: %8")
		.arg(videoStreamIndex).arg(videoDecoder->name).arg(ifmt_ctx->iformat->name)
		.arg((ifmt_ctx->duration) / 1000000).arg(videoFps).arg(videoWidth).arg(videoHeight).arg(url);
	qDebug() << TIMEMS << videoInfo;
	return true;
}

bool ScreenCapture::initDecoder()
{
	AVCodec* deCodec = NULL;
	deCodecCtx = in_stream->codec;
	deCodec = avcodec_find_decoder(deCodecCtx->codec_id);

	int result = avcodec_open2(deCodecCtx, deCodec, NULL);
	if (result < 0) {
		qDebug() << TIMEMS << "open video codec error :" << result;
		return false;
	}

	return true;
}

void ScreenCapture::memoryAlloc()
{
	//分配AVFram及像素存储空间
	avFrameRgb = av_frame_alloc();
	avFrameRgb->format = srcFormat;
	avFrameRgb->width = videoWidth;
	avFrameRgb->height = videoHeight;
	av_frame_get_buffer(avFrameRgb, 32);

	avFrameYuv = av_frame_alloc();
	avFrameYuv->format = dstFormat;
	avFrameYuv->width = videoWidth;
	avFrameYuv->height = videoHeight;
	av_frame_get_buffer(avFrameYuv, 32);

	avEnPacket = av_packet_alloc();
	av_init_packet(avEnPacket);
	avDePacket = av_packet_alloc();
	av_init_packet(avDePacket);
}

bool ScreenCapture::initSwsContext()
{
	/* 定义像素格式 */
	srcFormat = AV_PIX_FMT_BGRA;
	dstFormat = AV_PIX_FMT_YUV420P;
	/* 默认最快速度的解码采用的SWS_FAST_BILINEAR参数 */
	int flags = SWS_FAST_BILINEAR;
	/* 格式转换上下文(RgbtoYuv) */
	swsContextRgbtoYuv = sws_getContext(videoWidth, videoHeight, srcFormat, \
		videoWidth, videoHeight, dstFormat, \
		flags, NULL, NULL, NULL);

	return true;
}

bool ScreenCapture::initOutputStream()
{
	/* 设置输出封装格式上下文 */
	avformat_alloc_output_context2(&ofmt_ctx, 0, "mpegts", fileName.toLatin1().data());
	ofmt = ofmt_ctx->oformat;
	/* 封装格式上下文中增加视频流信息 */
	out_stream = avformat_new_stream(ofmt_ctx, NULL);
	/* 设置封装器参数 */
	out_stream->id = 0;
	out_stream->codecpar->codec_tag = 0;
	avcodec_parameters_from_context(out_stream->codecpar, enCodecCtx);
	av_dump_format(ofmt_ctx, 0, fileName.toLatin1().data(), 1);

	/* 写MP4头 */
	avio_open(&ofmt_ctx->pb, fileName.toLatin1().data(), AVIO_FLAG_WRITE);
	int ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0)
	{
		qDebug() << "Failed to avformat_write_header.";
		return false;
	}

	return true;
}

bool ScreenCapture::initEncoder()
{
	/* 找到编码器对象 */
	AVCodec* encodec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!encodec)
	{
		qDebug() << "Failed to find encoder.";
		return false;
	}
	/* 分配编码器上下文 */
	enCodecCtx = avcodec_alloc_context3(encodec);
	if (!enCodecCtx)
	{
		qDebug() << "Failed to alloc context3.";
		return false;
	}
	/* 设置编码器参数 */
	enCodecCtx->bit_rate = 400000;
	enCodecCtx->width = videoWidth;
	enCodecCtx->height = videoHeight;
	enCodecCtx->time_base = { 1,videoFps };
	enCodecCtx->framerate = { videoFps,1 };
	enCodecCtx->gop_size = 50;
	enCodecCtx->keyint_min = 20;
	enCodecCtx->max_b_frames = 0;
	enCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
	enCodecCtx->codec_id = AV_CODEC_ID_H264;
	enCodecCtx->flags |= AV_CODEC_FLAG2_LOCAL_HEADER;
	enCodecCtx->thread_count = 8;
	/* 量化因子,范围越大，画质越差，编码速度越快 */
	enCodecCtx->qmin = 20;
	enCodecCtx->qmax = 30;

	AVDictionary* param = 0;
	av_dict_set(&param, "preset", "superfast", 0);
	av_dict_set(&param, "tune", "zerolatency", 0);

	/* 打开编码器 */
	int ret = avcodec_open2(enCodecCtx, encodec, &param);
	if (ret < 0)
	{
		qDebug() << "Failed to open enCodecCtx.";
		return false;
	}

	qDebug() << TIMEMS;
	return true;
}

void ScreenCapture::setStop(bool stop)
{
	this->stop = stop;
}
