#ifndef PSIACCOUNTCONTROLLINGHOST_H
#define PSIACCOUNTCONTROLLINGHOST_H


class PsiAccountControllingHost
{
public:
	virtual ~PsiAccountControllingHost() {}

	virtual void setStatus(int account, const QString& status, const QString& statusMessage) = 0;
};

Q_DECLARE_INTERFACE(PsiAccountControllingHost, "org.psi-im.PsiAccountControllingHost/0.1");

#endif // PSIACCOUNTCONTROLLINGHOST_H
