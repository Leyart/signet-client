#ifndef ESDBBOOKMARKMODULE_H
#define ESDBBOOKMARKMODULE_H

#include <QObject>

#include "esdbtypemodule.h"

class LoggedInWidget;

class esdbBookmarkModule : public esdbTypeModule
{
private:
	LoggedInWidget *m_parent;
	esdbEntry *decodeEntry(int id, int revision, esdbEntry *prev, struct block *blk) const;
	esdbEntry *decodeEntry(const QVector<genericField> &fields, bool aliasMatch = true) const;
public:
	esdbBookmarkModule(LoggedInWidget *parent) :
		esdbTypeModule("Bookmarks"),
		m_parent(parent)
	{

	}
};

#endif // ESDBBOOKMARKMODULE_H
