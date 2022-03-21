#include <QtCore/QCoreApplication>
#include "screencapture.h"

#include <iostream>

using namespace std;

int main(int argc, char *argv[])
{
	QCoreApplication a(argc, argv);
	ScreenCapture* capture = new ScreenCapture();
	capture->start();
	while (true)
	{
		string str;
		cin >> str;
		if (str == "q")
		{
			capture->setStop(true);
			break;
		}
	}
	return a.exec();
}
