#include <QObject>
#include <QtTest/QtTest>

#include "qttestutil/qttestutil.h"

class MySecondClassTest : public QObject
{
     Q_OBJECT
	
	private slots:
		void initTestCase() {
		}

		void cleanupTestCase() {
		}

		void testMyMethod() {
			QCOMPARE(1, 0); // Dummy test
		}
};

QTTESTUTIL_REGISTER_TEST(MySecondClassTest);
#include "mysecondclasstest.moc"
