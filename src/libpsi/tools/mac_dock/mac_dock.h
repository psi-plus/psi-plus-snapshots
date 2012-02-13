#ifndef MACDOCK_H
#define MACDOCK_H

#include <QString>

class MacDock
{
public:
	static void startBounce();
	static void stopBounce();
	static void overlay(const QString& text = QString::null);

private:
	static bool isBouncing;
	static bool overlayed;
};

#endif
