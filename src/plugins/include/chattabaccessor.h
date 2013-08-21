#ifndef CHATTABACCESSOR_H
#define CHATTABACCESSOR_H

class QWidget;
class QString;

class ChatTabAccessor
{
public:
	virtual ~ChatTabAccessor() {}

	virtual void setupChatTab(QWidget* tab, int account, const QString& contact) = 0;
	virtual void setupGCTab(QWidget* tab, int account, const QString& contact) = 0;
};

Q_DECLARE_INTERFACE(ChatTabAccessor, "org.psi-im.ChatTabAccessor/0.1");

#endif // CHATTABACCESSOR_H
