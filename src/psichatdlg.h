#ifndef PSICHATDLG_H
#define PSICHATDLG_H

#include "minicmd.h"
#include "mcmdsimplesite.h"
#include "mcmdmanager.h"
#include "chatdlg.h"
#include "mcmdcompletion.h"
#include "applicationinfo.h"

#include "ui_chatdlg.h"
#include "typeaheadfind.h"
#include "psiwindowheader.h"
#include "widgets/actionlineedit.h"
#include "actionlist.h"

class IconAction;
class PsiContact;

class PsiChatDlg : public ChatDlg
{
	Q_OBJECT
public:
	PsiChatDlg(const Jid& jid, PsiAccount* account, TabManager* tabManager);
	~PsiChatDlg();

	virtual void setVSplitterPosition(int log,int chat);

protected:
	// reimplemented
	void contextMenuEvent(QContextMenuEvent *);
	void doSend();
	bool eventFilter(QObject *obj, QEvent *event);
	void ackLastMessages(int msgs);

private:
	void setContactToolTip(QString text);

private slots:
	void toggleSmallChat();
	void doClearButton();
	void doMiniCmd();
	void addContact();
	void doMinimize();
	void buildMenu();
	void updateCounter();
	void updateIdentityVisibility();
	void updateCountVisibility();
	void updateContactAdding(PsiContact* c = 0);
	void updateContactAdding(const Jid &j);
	void updateToolbuttons();
	void copyUserJid();

	// reimplemented
	void chatEditCreated();
	void sendButtonMenu();
	void editTemplates();
	void doPasteAndSend();
	void sendTemp(const QString &);
	void psButtonEnabled();
	void verticalSplitterMoved(int, int);
	void contactChanged();
	QString makeContactName(const QString &name, const Jid &jid) const;
	void doSwitchJidMode();
	void actActiveContacts();
	void actPgpToggled(bool);

private:
	void initToolBar();
	void initToolButtons();

	// reimplemented
	void initUi();
	void capsChanged();
	bool isEncryptionEnabled() const;
	void updateJidWidget(const QList<UserListItem*> &ul, int status, bool fromPresence);
	void contactUpdated(UserListItem* u, int status, const QString& statusString);
	void updateAvatar();
	void optionsUpdate();
	void updatePGP();
	void checkPGPAutostart();
	void setPGPEnabled(bool enabled);
	void activated();
	void setLooks();
	void setShortcuts();
	void appendSysMsg(const QString &);
	ChatView* chatView() const;
	ChatEdit* chatEdit() const;
	void setMargins();
	void updateAutojidIcon();
	void setJidComboItem(int pos, const QString &text, const Jid &jid, const QString &icon_str);

private:
	Ui::ChatDlg ui_;

	QMenu* pm_settings_;

	ActionList* actions_;
	TypeAheadFindBar *typeahead;
	QAction *act_mini_cmd_, *act_minimize_;

	ActionLineEdit *le_autojid;
	IconAction *act_autojid;
	IconAction *act_active_contacts;

	MCmdManager mCmdManager_;
	MCmdSimpleSite mCmdSite_;

	MCmdTabCompletion tabCompletion;

	bool smallChat_;
	class ChatDlgMCmdProvider;

	const PsiIcon *current_status_icon;
	QString last_contact_tooltip;
	int unacked_messages;

	static QMovie *throbber_movie;

	int logHeight;
	int chateditHeight;

	bool autoPGP_;

private:
	bool tabmode;
	QPointer <PsiWindowHeader> winHeader_;
};

#endif
