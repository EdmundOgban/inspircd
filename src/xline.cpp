/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2007 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* $Core: libIRCDxline */

#include "inspircd.h"
#include "wildcard.h"
#include "xline.h"

/*
 * This is now version 3 of the XLine subsystem, let's see if we can get it as nice and 
 * efficient as we can this time so we can close this file and never ever touch it again ..
 *
 * Background:
 *  Version 1 stored all line types in one list (one for g, one for z, etc). This was fine,
 *  but both version 1 and 2 suck at applying lines efficiently. That is, every time a new line
 *  was added, it iterated every existing line for every existing user. Ow. Expiry was also
 *  expensive, as the lists were NOT sorted.
 *
 *  Version 2 moved permanent lines into a seperate list from non-permanent to help optimize
 *  matching speed, but matched in the same way.
 *  Expiry was also sped up by sorting the list by expiry (meaning just remove the items at the
 *  head of the list that are outdated.)
 *
 * This was fine and good, but it looked less than ideal in code, and matching was still slower
 * than it could have been, something which we address here.
 *
 * VERSION 3:
 *  All lines are (as in v1) stored together -- no seperation of perm and non-perm. Expiry will
 *  still use a sorted list, and we'll just ignore anything permanent.
 *
 *  Application will be by a list of lines 'pending' application, meaning only the newly added lines
 *  will be gone over. Much faster.
 *
 * More of course is to come.
 */

/* Version two, now with optimized expiry!
 *
 * Because the old way was horrendously slow, the new way of expiring xlines is very
 * very efficient. I have improved the efficiency of the algorithm in two ways:
 *
 * (1) There are now two lists of items for each linetype. One list holds temporary
 *     items, and the other list holds permanent items (ones which will expire).
 *     Items which are on the permanent list are NEVER checked at all by the
 *     expire_lines() function.
 * (2) The temporary xline lists are always kept in strict numerical order, keyed by
 *     current time + duration. This means that the line which is due to expire the
 *     soonest is always pointed at by vector::begin(), so a simple while loop can
 *     very efficiently, very quickly and above all SAFELY pick off the first few
 *     items in the vector which need zapping.
 *
 *     -- Brain
 */

bool XLine::Matches(User *u)
{
	return false;
}

/*
 * Checks what users match a given vector of ELines and sets their ban exempt flag accordingly.
 */
void XLineManager::CheckELines(std::map<std::string, XLine *> &ELines)
{
	if (ELines.empty())
		return;

	for (std::vector<User*>::const_iterator u2 = ServerInstance->local_users.begin(); u2 != ServerInstance->local_users.end(); u2++)
	{
		User* u = (User*)(*u2);

		for (std::map<std::string, XLine *>::iterator i = ELines.begin(); i != ELines.end(); i++)
		{
			XLine *e = i->second;
			u->exempt = e->Matches(u);
		}
	}
}

// this should probably be moved to configreader, but atm it relies on CheckELines above.
bool DoneELine(ServerConfig* conf, const char* tag)
{
	for (std::vector<User*>::const_iterator u2 = conf->GetInstance()->local_users.begin(); u2 != conf->GetInstance()->local_users.end(); u2++)
	{
		User* u = (User*)(*u2);
		u->exempt = false;
	}

	conf->GetInstance()->XLines->CheckELines(conf->GetInstance()->XLines->lookup_lines['E']);
	return true;
}


IdentHostPair XLineManager::IdentSplit(const std::string &ident_and_host)
{
	IdentHostPair n = std::make_pair<std::string,std::string>("*","*");
	std::string::size_type x = ident_and_host.find('@');
	if (x != std::string::npos)
	{
		n.second = ident_and_host.substr(x + 1,ident_and_host.length());
		n.first = ident_and_host.substr(0, x);
		if (!n.first.length())
			n.first.assign("*");
		if (!n.second.length())
			n.second.assign("*");
	}
	else
	{
		n.second = ident_and_host;
	}

	return n;
}

// adds a g:line

bool XLineManager::AddGLine(long duration, const char* source,const char* reason,const char* hostmask)
{
	IdentHostPair ih = IdentSplit(hostmask);

	if (DelLine(hostmask, 'G', true))
		return false;

	GLine* item = new GLine(ServerInstance, ServerInstance->Time(), duration, source, reason, ih.first.c_str(), ih.second.c_str());

	active_lines.push_back(item);
	sort(active_lines.begin(), active_lines.end(),XLineManager::XSortComparison);
	pending_lines.push_back(item);
	lookup_lines['G'][hostmask] = item;

	return true;
}

// adds an e:line (exception to bans)

bool XLineManager::AddELine(long duration, const char* source, const char* reason, const char* hostmask)
{
	IdentHostPair ih = IdentSplit(hostmask);

	if (DelLine(hostmask, 'E', true))
		return false;

	ELine* item = new ELine(ServerInstance, ServerInstance->Time(), duration, source, reason, ih.first.c_str(), ih.second.c_str());

	active_lines.push_back(item);
	sort(active_lines.begin(), active_lines.end(),XLineManager::XSortComparison);
	lookup_lines['E'][hostmask] = item;

	// XXX we really only need to check one line (the new one) - this is a bit wasteful!
	// we should really create a temporary var here and pass that instead.
	// hmm, perhaps we can merge this with line "application" somehow.. and just force a recheck on DelELine?
	this->CheckELines(lookup_lines['E']);
	return true;
}

// adds a q:line

bool XLineManager::AddQLine(long duration, const char* source, const char* reason, const char* nickname)
{
	if (DelLine(nickname, 'Q', true))
		return false;

	QLine* item = new QLine(ServerInstance, ServerInstance->Time(), duration, source, reason, nickname);

	active_lines.push_back(item);
	sort(active_lines.begin(), active_lines.end(), XLineManager::XSortComparison);
	pending_lines.push_back(item);
	lookup_lines['Q'][nickname] = item;

	return true;
}

// adds a z:line

bool XLineManager::AddZLine(long duration, const char* source, const char* reason, const char* ipaddr)
{
	if (strchr(ipaddr,'@'))
	{
		while (*ipaddr != '@')
			ipaddr++;
		ipaddr++;
	}

	if (DelLine(ipaddr, 'Z', true))
		return false;

	ZLine* item = new ZLine(ServerInstance, ServerInstance->Time(), duration, source, reason, ipaddr);

	active_lines.push_back(item);
	sort(active_lines.begin(), active_lines.end(),XLineManager::XSortComparison);
	pending_lines.push_back(item);
	lookup_lines['Z'][ipaddr] = item;

	return true;
}

// adds a k:line

bool XLineManager::AddKLine(long duration, const char* source, const char* reason, const char* hostmask)
{
	IdentHostPair ih = IdentSplit(hostmask);

	if (DelLine(hostmask, 'K', true))
		return false;

	KLine* item = new KLine(ServerInstance, ServerInstance->Time(), duration, source, reason, ih.first.c_str(), ih.second.c_str());

	active_lines.push_back(item);
	sort(active_lines.begin(), active_lines.end(),XLineManager::XSortComparison);
	pending_lines.push_back(item);
	lookup_lines['K'][hostmask] = item;

	return true;
}

// deletes a g:line, returns true if the line existed and was removed

bool XLineManager::DelLine(const char* hostmask, char type, bool simulate)
{
	IdentHostPair ih = IdentSplit(hostmask);
	for (std::vector<XLine*>::iterator i = active_lines.begin(); i != active_lines.end(); i++)
	{
		if ((*i)->type == type)
		{
			if ((*i)->Matches(hostmask))
			{
				if (!simulate)
				{
					(*i)->Unset();
					delete *i;
					active_lines.erase(i);
					if (lookup_lines.find(type) != lookup_lines.end())
						lookup_lines[type].erase(hostmask);
					/* XXX: Should erase from pending lines here */
				}
				return true;
			}
		}
	}

	return false;
}


void ELine::Unset()
{
	/* remove exempt from everyone and force recheck after deleting eline */
	for (std::vector<User*>::const_iterator u2 = ServerInstance->local_users.begin(); u2 != ServerInstance->local_users.end(); u2++)
	{
		User* u = (User*)(*u2);
		u->exempt = false;
	}

	if (ServerInstance->XLines->lookup_lines.find('E') != ServerInstance->XLines->lookup_lines.end())
		ServerInstance->XLines->CheckELines(ServerInstance->XLines->lookup_lines['E']);
}

// returns a pointer to the reason if a nickname matches a qline, NULL if it didnt match

QLine* XLineManager::matches_qline(const char* nick)
{
	if (lookup_lines.find('Q') == lookup_lines.end())
		return NULL;

	if (lookup_lines.find('Q') != lookup_lines.end() && lookup_lines['Q'].empty())
		return NULL;

	for (std::vector<XLine*>::iterator i = active_lines.begin(); i != active_lines.end(); i++)
		if ((*i)->type == 'Q' && (*i)->Matches(nick))
			return (QLine*)(*i);
	return NULL;
}

// returns a pointer to the reason if a host matches a gline, NULL if it didnt match

GLine* XLineManager::matches_gline(User* user)
{
	if (lookup_lines.find('G') == lookup_lines.end())
		return NULL;

	if (lookup_lines.find('G') != lookup_lines.end() && lookup_lines['G'].empty())
		return NULL;

	for (std::vector<XLine*>::iterator i = active_lines.begin(); i != active_lines.end(); i++)
		if ((*i)->type == 'G' && (*i)->Matches(user))
			return (GLine*)(*i);

	return NULL;
}

ELine* XLineManager::matches_exception(User* user)
{
	if (lookup_lines.find('E') == lookup_lines.end())
		return NULL;

	if (lookup_lines.find('E') != lookup_lines.end() && lookup_lines['E'].empty())
		return NULL;

	for (std::vector<XLine*>::iterator i = active_lines.begin(); i != active_lines.end(); i++)
	{
		if ((*i)->type == 'E' && (*i)->Matches(user))
			return (ELine*)(*i);
	}
	return NULL;
}


void XLineManager::gline_set_creation_time(const char* host, time_t create_time)
{
	/*for (std::vector<XLine*>::iterator i = glines.begin(); i != glines.end(); i++)
	{
		if (!strcasecmp(host,(*i)->hostmask))
		{
			(*i)->set_time = create_time;
			(*i)->expiry = create_time + (*i)->duration;
			return;
		}
	}*/

	return ;
}

void XLineManager::eline_set_creation_time(const char* host, time_t create_time)
{
	/*for (std::vector<ELine*>::iterator i = elines.begin(); i != elines.end(); i++)
	{
		if (!strcasecmp(host,(*i)->hostmask))
		{
			(*i)->set_time = create_time;
			(*i)->expiry = create_time + (*i)->duration;
			return;
		}
	}*/

	return;
}

void XLineManager::qline_set_creation_time(const char* nick, time_t create_time)
{
	/*for (std::vector<QLine*>::iterator i = qlines.begin(); i != qlines.end(); i++)
	{
		if (!strcasecmp(nick,(*i)->nick))
		{
			(*i)->set_time = create_time;
			(*i)->expiry = create_time + (*i)->duration;
			return;
		}
	}*/

	return;
}

void XLineManager::zline_set_creation_time(const char* ip, time_t create_time)
{
	/*for (std::vector<ZLine*>::iterator i = zlines.begin(); i != zlines.end(); i++)
	{
		if (!strcasecmp(ip,(*i)->ipaddr))
		{
			(*i)->set_time = create_time;
			(*i)->expiry = create_time + (*i)->duration;
			return;
		}
	}*/

	return;
}

// returns a pointer to the reason if an ip address matches a zline, NULL if it didnt match

ZLine* XLineManager::matches_zline(User *u)
{
	if (lookup_lines.find('Z') == lookup_lines.end())
		return NULL;

	if (lookup_lines.find('Z') != lookup_lines.end() && lookup_lines['Z'].empty())
		return NULL;

	for (std::vector<XLine*>::iterator i = active_lines.begin(); i != active_lines.end(); i++)
		if ((*i)->type == 'Z' && (*i)->Matches(u))
			return (ZLine*)(*i);
	return NULL;
}

// returns a pointer to the reason if a host matches a kline, NULL if it didnt match

KLine* XLineManager::matches_kline(User* user)
{
	if (lookup_lines.find('K') == lookup_lines.end())
		return NULL;

	if (lookup_lines.find('K') != lookup_lines.end() && lookup_lines['K'].empty())
		return NULL;

	for (std::vector<XLine*>::iterator i = active_lines.begin(); i != active_lines.end(); i++)
		if ((*i)->Matches(user))
			return (KLine*)(*i);

	return NULL;
}

bool XLineManager::XSortComparison(const XLine *one, const XLine *two)
{
	// account for permanent lines
	if (one->expiry == 0)
	{
		return false;
	}
	return (one->expiry) < (two->expiry);
}

// removes lines that have expired
void XLineManager::expire_lines()
{
	time_t current = ServerInstance->Time();

	/* Because we now store all our XLines in sorted order using ((*i)->duration + (*i)->set_time) as a key, this
	 * means that to expire the XLines we just need to do a while, picking off the top few until there are
	 * none left at the head of the queue that are after the current time.
	 */

	while ((active_lines.size()) && (current > (*active_lines.begin())->expiry) && ((*active_lines.begin())->duration != 0))
	{
		std::vector<XLine*>::iterator i = active_lines.begin();
		(*i)->DisplayExpiry();
		(*i)->Unset();
		active_lines.erase(i);
		delete *i;
	}
}

// applies lines, removing clients and changing nicks etc as applicable
void XLineManager::ApplyLines()
{
	for (std::vector<User*>::const_iterator u2 = ServerInstance->local_users.begin(); u2 != ServerInstance->local_users.end(); u2++)
	{
		User* u = (User*)(*u2);

		for (std::vector<XLine *>::iterator i = pending_lines.begin(); i != pending_lines.end(); i++)
		{
			XLine *x = *i;
			if (x->Matches(u))
				x->Apply(u);
		}
	}

	pending_lines.clear();
}

void XLineManager::stats_k(User* user, string_list &results)
{
	/*std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<KLine*>::iterator i = klines.begin(); i != klines.end(); i++)
		results.push_back(sn+" 216 "+user->nick+" :"+(*i)->identmask+"@"+(*i)->hostmask+" "+ConvToStr((*i)->set_time)+" "+ConvToStr((*i)->duration)+" "+(*i)->source+" :"+(*i)->reason);*/
}

void XLineManager::stats_g(User* user, string_list &results)
{
	/*std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<GLine*>::iterator i = glines.begin(); i != glines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+(*i)->identmask+"@"+(*i)->hostmask+" "+ConvToStr((*i)->set_time)+" "+ConvToStr((*i)->duration)+" "+(*i)->source+" :"+(*i)->reason);*/
}

void XLineManager::stats_q(User* user, string_list &results)
{
	/*std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<QLine*>::iterator i = qlines.begin(); i != qlines.end(); i++)
		results.push_back(sn+" 217 "+user->nick+" :"+(*i)->nick+" "+ConvToStr((*i)->set_time)+" "+ConvToStr((*i)->duration)+" "+(*i)->source+" :"+(*i)->reason);*/
}

void XLineManager::stats_z(User* user, string_list &results)
{
	/*std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<ZLine*>::iterator i = zlines.begin(); i != zlines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+(*i)->ipaddr+" "+ConvToStr((*i)->set_time)+" "+ConvToStr((*i)->duration)+" "+(*i)->source+" :"+(*i)->reason);*/
}

void XLineManager::stats_e(User* user, string_list &results)
{
	/*std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<ELine*>::iterator i = elines.begin(); i != elines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+(*i)->identmask+"@"+(*i)->hostmask+" "+ConvToStr((*i)->set_time)+" "+ConvToStr((*i)->duration)+" "+(*i)->source+" :"+(*i)->reason);*/
}

XLineManager::XLineManager(InspIRCd* Instance) : ServerInstance(Instance)
{
}

bool XLine::Matches(const std::string &str)
{
	return false;
}

void XLine::Apply(User* u)
{
}

void XLine::DefaultApply(User* u, char line)
{
	char reason[MAXBUF];
	snprintf(reason, MAXBUF, "%c-Lined: %s", line, this->reason);
	if (*ServerInstance->Config->MoronBanner)
		u->WriteServ("NOTICE %s :*** %s", u->nick, ServerInstance->Config->MoronBanner);
	if (ServerInstance->Config->HideBans)
		User::QuitUser(ServerInstance, u, line + std::string("-Lined"), reason);
	else
		User::QuitUser(ServerInstance, u, reason);
}

bool KLine::Matches(User *u)
{
	if (u->exempt)
		return false;

	if ((match(u->ident, this->identmask)))
	{
		if ((match(u->host, this->hostmask, true)) || (match(u->GetIPString(), this->hostmask, true)))
		{
			return true;
		}
	}

	return false;
}

void KLine::Apply(User* u)
{
	DefaultApply(u, 'K');
}

bool GLine::Matches(User *u)
{
	if (u->exempt)
		return false;

	if ((match(u->ident, this->identmask)))
	{
		if ((match(u->host, this->hostmask, true)) || (match(u->GetIPString(), this->hostmask, true)))
		{
			return true;
		}
	}

	return false;
}

void GLine::Apply(User* u)
{       
	DefaultApply(u, 'G');
}

bool ELine::Matches(User *u)
{
	if (u->exempt)
		return false;

	if ((match(u->ident, this->identmask)))
	{
		if ((match(u->host, this->hostmask, true)) || (match(u->GetIPString(), this->hostmask, true)))
		{
			return true;
		}
	}

	return false;
}

bool ZLine::Matches(User *u)
{
	if (u->exempt)
		return false;

	if (match(u->GetIPString(), this->ipaddr, true))
		return true;
	else
		return false;
}

void ZLine::Apply(User* u)
{       
	DefaultApply(u, 'Z');
}


bool QLine::Matches(User *u)
{
	if (u->exempt)
		return false;

	if (match(u->nick, this->nick))
		return true;

	return false;
}

void QLine::Apply(User* u)
{       
	/* Can we force the user to their uid here instead? */
	DefaultApply(u, 'Q');
}


bool ZLine::Matches(const std::string &str)
{
	if (match(str.c_str(), this->ipaddr, true))
		return true;
	else
		return false;
}

bool QLine::Matches(const std::string &str)
{
	if (match(str.c_str(), this->nick))
		return true;

	return false;
}

void ELine::DisplayExpiry()
{
	ServerInstance->SNO->WriteToSnoMask('x',"Expiring timed E-Line %s@%s (set by %s %d seconds ago)",this->identmask,this->hostmask,this->source,this->duration);
}

void QLine::DisplayExpiry()
{
	ServerInstance->SNO->WriteToSnoMask('x',"Expiring timed G-Line %s (set by %s %d seconds ago)",this->nick,this->source,this->duration);
}

void ZLine::DisplayExpiry()
{
	ServerInstance->SNO->WriteToSnoMask('x',"Expiring timed Z-Line %s (set by %s %d seconds ago)",this->ipaddr,this->source,this->duration);
}

void KLine::DisplayExpiry()
{
	ServerInstance->SNO->WriteToSnoMask('x',"Expiring timed K-Line %s@%s (set by %s %d seconds ago)",this->identmask,this->hostmask,this->source,this->duration);
}

void GLine::DisplayExpiry()
{
	ServerInstance->SNO->WriteToSnoMask('x',"Expiring timed G-Line %s@%s (set by %s %d seconds ago)",this->identmask,this->hostmask,this->source,this->duration);
}

