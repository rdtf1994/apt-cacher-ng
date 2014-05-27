
//#define LOCAL_DEBUG
#include "debug.h"

#include "expiration.h"
#include "meta.h"
#include "filereader.h"
#include "fileio.h"

#include <fstream>

#include <unistd.h>
#include <dirent.h>

#include <map>
#include <string>
#include <iostream>
#include <algorithm>

using namespace MYSTD;

#define ENABLED_

#ifndef ENABLED
#warning Unlinking parts defused
#endif

#define ERRMSGABORT if(m_nErrorCount && m_bErrAbort) { SendChunk(sErr); return; }
#define ERRABORT if(m_nErrorCount && m_bErrAbort) { return; }

#define TIMEEXPIRED(t) (t < (m_gMaintTimeNow-acfg::extreshhold*86400))
#define TIME_AGONY (m_gMaintTimeNow-acfg::extreshhold*86400)

#define FNAME_PENDING "_expending_dat"
#define FNAME_DAMAGED "_expending_damaged"
#define sFAIL_INI SABSPATH("_exfail_cnt")
#define FAIL_INI sFAIL_INI.c_str()

void expiration::HandlePkgEntry(const tRemoteFileInfo &entry)
{
#warning mit -1 size klar kommen
	LOGSTART2("expiration::_HandlePkgEntry:",
			"\ndir:" << entry.sDirectory <<
			"\nname: " << entry.sFileName <<
			"\nsize: " << entry.fpr.size <<
			"\ncsum: " << entry.fpr.GetCsAsString());

	auto rangeIt = m_trashFile2dir2Info.find(entry.sFileName);
	if(rangeIt == m_trashFile2dir2Info.end())
		return;

	for(auto it=rangeIt->second.begin(); it!=rangeIt->second.end();
			/* erases inside -> step forward there */ )
	{
		// shortcuts
		const auto& filenameHave = rangeIt->first;
		tDiskFileInfo &descHave = it->second;
		//tFingerprint & fprHave=descHave.fpr;

		string sPathRel(it->first + filenameHave);
		string sPathAbs(CACHE_BASE+sPathRel);

		if(ContHas(m_forceKeepInTrash,sPathRel))
			goto keep_in_trash;

		// needs to match the exact file location.
		// And for "Index" files, they have always to be at a well defined location, this
		// constraint is also needed to expire deprecated files
		if(m_bByPath || entry.sFileName == sIndex)
		{
			// compare full paths (physical vs. remote) with their real paths
			string sEntrPath=entry.sDirectory+entry.sFileName;
			LOG("Checking exact path: " << sEntrPath << " vs. " << sPathRel);
			pathTidy(sEntrPath);
			if(sPathRel != sEntrPath)
				goto keep_in_trash; // not for us, try another one
		}

		// Basic header checks. Skip if the file was forcibly updated/reconstructed before.
		if (m_bSkipHeaderChecks || descHave.bNoHeaderCheck)
		{
			LOG("Skipped header check for " << sPathRel);
		}
		else if(entry.bInflateForCs)
		{
			LOG("Skipped header check for " << sPathRel << " needs to be uncompressed first");
		}
		else if(entry.fpr.size>=0)
		{
			LOG("Doing basic header checks");
			header h;
			if (0<h.LoadFromFile(sPathAbs+".head"))
			{
				off_t len=atoofft(h.h[header::CONTENT_LENGTH], -2);
				if(len<0)
				{
					SendFmt << "<span class=\"ERROR\">WARNING, header file of "
							<< sPathRel << " does not contain content length</span>";
					goto keep_in_trash;
				}
				off_t lenInfo=GetFileSize(sPathAbs, -3);
				if(lenInfo<0)
				{
					SendFmt << ": error reading attributes, ignoring" << sPathRel;
					goto keep_in_trash;
				}
				if (len < lenInfo)
				{
					SendFmt << "<span class=\"ERROR\">WARNING, header file of " << sPathRel
							<< " reported too small file size (" << len << " vs. " << lenInfo
							<< "), invalidating and removing header</span><br>\n";
					// kill the header ASAP, no matter which extra exception might apply later
					ignore_value(::unlink((sPathAbs+".head").c_str()));
					goto keep_in_trash;
				}
			}
			else
			{
				tIfileAttribs at = GetFlags(sPathRel);
				if(!at.parseignore && !at.forgiveDlErrors)
				{
					SendFmt<<"<span class=\"WARNING\">WARNING, header file missing or damaged for "
						<<sPathRel<<"</span>\n<br>\n";
				}
			}
		}

		if(m_bByChecksum)
		{
			tFingerprint& fprCache = descHave.fpr;

			bool bSkipDataCheck=false;

			// can tell this ASAP if unpacking is not needed
			if(!entry.bInflateForCs &&
					fprCache.csType == entry.fpr.csType &&
					fprCache.size < entry.fpr.size)
			{
				if(m_bIncompleteIsDamaged)
				{
					if(m_bTruncateDamaged)
						goto l_tell_truncating;
					else
						goto l_tell_damaged;
				}
				goto l_tell_incomplete;
			}

			// scan file if not done before or the checksum type differs
			if(entry.fpr.csType != fprCache.csType
					&& !fprCache.ScanFile(sPathAbs, entry.fpr.csType, entry.bInflateForCs))
			{
					// IO error? better keep it for now
				aclog::err(tSS()<<"An error occurred while checksumming "
						<< sPathAbs	<< ", not touching it.");
				bSkipDataCheck=true;
			}

#warning fixme, total wirr, und eigentlich ist der fprcache auch schrott, sollte cstype beruecksichtigen

			if ( !bSkipDataCheck && fprCache != entry.fpr)
			{
				if (fprCache.size < entry.fpr.size && !m_bIncompleteIsDamaged)
				{
					l_tell_incomplete:
					SendFmt << "<span class=\"WARNING\">INCOMPLETE: " << sPathRel
							<< " (ignoring)</span><br>\n";
				}
				else if (m_bTruncateDamaged)
				{
					l_tell_truncating:
					SendFmt << "<span class=\"WARNING\">BAD: " << sPathRel
							<< " (truncating)</span><br>\n";
					ignore_value(::truncate(sPathAbs.c_str(), 0));
				}
				else
				{
					l_tell_damaged:

					if(GetFlags(sPathRel).parseignore)
					{
						SendFmt << sPathRel << ": incorrect or obsolete contents but marked as "
								"alternative version of another index file "
								"<span class=\"WARNING\">(ignoring)</span>";
					}
					else
					{
						if (m_damageList.is_open())
							m_damageList << sPathRel << endl;
						SendFmt << "<span class=\"ERROR\">BAD: " << sPathRel
								<< " (adding to damage list)</span>";
						AddDelCbox(sPathRel);
					}
					SendChunk("<br>\n");

					goto keep_in_trash;
				}
			}
		}

		// ok, package matched, contents ok if checked, drop it from the removal list
		if (m_bVerbose)
			SendFmt << "<font color=green>OK: " << sPathRel << "</font><br>\n";

		// update usage statistics
		SetFlags(m_processedIfile).space+=entry.fpr.size;

		rangeIt->second.erase(it++);

		continue;

		keep_in_trash:
		it++;
	}
}

// this method looks for the validity of additional package files kept in cache after
// the Debian version moved to a higher one. Still a very simple algorithm and may not work
// as expected when there are multiple Debian/Blends/Ubuntu/GRML/... branches inside with lots
// of gaps in the "package history" when proceeded in linear fashion.
inline void expiration::DropExceptionalVersions()
{
    if(m_trashFile2dir2Info.empty() || !acfg::keepnver)
    	return;
    if(system("dpkg --version >/dev/null 2>&1"))
	{
		SendFmt << "dpkg not available on this system, cannot identify latest versions to keep "
				"only " << acfg::keepnver << " of them.";
		return;
    }
    struct tPkgId
    {
    	mstring prevName, ver, prevArcSufx;
    	map<mstring,tDiskFileInfo>* group;
    	~tPkgId() {group=0;};

    	inline bool Set(cmstring& fileName, decltype(group) newgroup)
    	{
    		group = newgroup;
    		tSplitWalk split(&fileName, "_");
    		if(!split.Next())
    			return false;
    		prevName=split;
    		if(!split.Next())
    			return false;
    		ver=split;
    		for (const char *p = prevArcSufx.c_str(); *p; ++p)
    			if (!isalnum(UINT(*p)) && !strchr(".-+:~", UINT(*p)))
    				return false;
    		if(!split.Next())
    			return false;
    		prevArcSufx=split;
    		return !split.Next(); // no trailing crap there
    	}
    	inline bool SamePkg(tPkgId &other) const
    	{
    		return other.prevName == prevName && other.prevArcSufx == prevArcSufx;
    	}
    	// move highest versions to beginning
    	inline bool operator<(const tPkgId &other) const
    	{
    		int r=::system((string("dpkg --compare-versions ")+ver+" gt "+other.ver).c_str());
    		return 0==r;
    	}
    };
    vector<tPkgId> version2trashGroup;
    auto procGroup = [&]()
		{
    	// if more than allowed, keep the highest versions for sure, others are expired as usual
    	if(version2trashGroup.size() > (UINT) acfg::keepnver)
        	std::sort(version2trashGroup.begin(), version2trashGroup.end());
    	for(UINT i=0; i<version2trashGroup.size() && i<UINT(acfg::keepnver); i++)
    		for(auto& j: * version2trashGroup[i].group)
    			j.second.nLostAt=m_gMaintTimeNow;
    	version2trashGroup.clear();
		};
    for(auto& trashFile2Group : m_trashFile2dir2Info)
    {
    	if(!endsWithSzAr(trashFile2Group.first, ".deb"))
			continue;
    	tPkgId newkey;
    	if(!newkey.Set(trashFile2Group.first, &trashFile2Group.second))
    		continue;
    	if(!version2trashGroup.empty() && !newkey.SamePkg(version2trashGroup.back()))
    		procGroup();
    	version2trashGroup.push_back(newkey);
    }
    if(!version2trashGroup.empty())
    	procGroup();
}

inline void expiration::RemoveAndStoreStatus(bool bPurgeNow)
{
	LOGSTART("expiration::_RemoveAndStoreStatus");
	FILE_RAII f;
    if(!bPurgeNow)
    {

        DropExceptionalVersions();

        string sDbFileAbs=CACHE_BASE+FNAME_PENDING;

        f.p = fopen(sDbFileAbs.c_str(), "w");
        if(!f)
        {
            SendChunk("Unable to open " FNAME_PENDING " for writing, attempting to recreate... ");
            ::unlink(sDbFileAbs.c_str());
            f.p=::fopen(sDbFileAbs.c_str(), "w");
            if(f)
                SendChunk("OK\n<br>\n");
            else
            {
                SendChunk("<span class=\"ERROR\">FAILED. ABORTING. Check filesystem and file permissions.</span>");
                return;
            }
        }
    }

	int nCount(0);
	off_t tagSpace(0);

	for (auto& fileGroup : m_trashFile2dir2Info)
	{
		for (auto& dir_props : fileGroup.second)
		{
			string sPathRel = dir_props.first + fileGroup.first;
			auto& desc = dir_props.second;
			DBGQLOG("Checking " << sPathRel);
			using namespace rechecks;

			if (ContHas(m_forceKeepInTrash, sPathRel))
			{
				LOG("forcetrash flag set, whitelist does not apply, not to be removed");
			}
			else
			{
				if (Match(fileGroup.first, WHITELIST) || Match(sPathRel, WHITELIST))
				{
					LOG("Protected file, not to be removed");
					continue;
				}
			}
			if (dir_props.second.nLostAt<=0) // heh, accidentally added?
				continue;
			//cout << "Unreferenced: " << it->second.sDirname << it->first <<endl;

			string sPathAbs = SABSPATH(sPathRel);

			if (bPurgeNow || TIMEEXPIRED(dir_props.second.nLostAt))
			{
				SendFmt << "Removing " << sPathRel << "<br>\n";

#ifdef ENABLED
				::unlink(sPathAbs.c_str());
				::unlink((sPathAbs + ".head").c_str());
				::rmdir(SZABSPATH(dir_props.first));
#endif
			}
			else if (f)
			{
				SendFmt << "Tagging " << sPathRel;
				if (m_bVerbose)
					SendFmt << " (t-" << (m_gMaintTimeNow - desc.nLostAt) / 3600 << "h)";
				SendChunk("<br>\n");

				nCount++;
				tagSpace += desc.fpr.size;
				fprintf(f, "%lu\t%s\t%s\n", desc.nLostAt,
						dir_props.first.c_str(),
						fileGroup.first.c_str());
			}
		}
	}
    if(nCount)
    	TellCount(nCount, tagSpace);
}


void expiration::Action(const string & cmd)
{
	if (m_mode==workExPurge)
	{
		LoadPreviousData(true);
		RemoveAndStoreStatus(true);
		return;
	}
	if (m_mode==workExList)
	{
		LoadPreviousData(true);
		off_t nSpace(0);
		uint cnt(0);
		for (auto& i : m_trashFile2dir2Info)
		{
			for (auto& j : i.second)
			{
				//const tFileNdir &k=it->first;
				auto rel = (j.first + i.first);
				auto abspath = SABSPATH(rel);
				off_t sz = GetFileSize(abspath, -2);
				if (sz < 0)
					continue;

				cnt++;
				SendChunk(rel + "<br>\n");
				nSpace += sz;

				sz = GetFileSize(abspath + ".head", -2);
				if (sz >= 0)
				{
					nSpace += sz;
					SendChunk(rel + ".head<br>\n");
				}
			}
		}
		TellCount(cnt, nSpace);

		mstring delURL(cmd);
		StrSubst(delURL, "justShow", "justRemove");
		SendFmtRemote << "<a href=\""<<delURL<<"\">Delete all listed files</a> "
				"(no further confirmation)<br>\n";
		return;
	}

	if(m_mode==workExPurgeDamaged || m_mode==workExListDamaged || m_mode==workExTruncDamaged)
	{
		filereader f;
		if(!f.OpenFile(SABSPATH(FNAME_DAMAGED)))
		{
			SendChunk("List of damaged files not found");
			return;
		}
		mstring s;
		while(f.GetOneLine(s))
		{
			if(s.empty())
				continue;

			if(m_mode == workExPurgeDamaged)
			{
				SendFmt << "Removing " << s << "<br>\n";
				::unlink(SZABSPATH(s));
				::unlink(SZABSPATH(s+".head"));
			}
			else if(m_mode == workExTruncDamaged)
			{
				SendFmt << "Truncating " << s << "<br>\n";
				ignore_value(::truncate(SZABSPATH(s), 0));
			}
			else
				SendFmt << s << "<br>\n";
		}
		return;
	}

	SetCommonUserFlags(cmd);

	m_bIncompleteIsDamaged=(cmd.find("incomAsDamaged")!=stmiss);

	SendChunk("<b>Locating potentially expired files in the cache...</b><br>\n");

	//dump_proc_status();
	DirectoryWalk(acfg::cachedir, this);
	//dump_proc_status();

	if(CheckStopSignal())
		goto save_fail_count;
	SendFmt<<"Found "<<m_nProgIdx<<" files.<br />\n";

#ifdef DEBUG
	for(auto& i: m_trashFile2dir2Info)
	{
		SendFmt << "<br>File: " << i.first <<"<br>\n";
		for(auto& j: i.second)
			 SendFmt << "Dir: " << j.first << " [ "<<j.second.fpr.size << " / " << j.second.nLostAt << " ]<br>\n";
	}
#endif

/*	if(m_bByChecksum)
		m_fprCache.rehash(1.25*m_trashFile2dir2Info.size());
*/
	//cout << "found package files: " << m_trashCandidates.size()<<endl;
	//for(tS2DAT::iterator it=m_trashCandSet.begin(); it!=m_trashCandSet.end(); it++)
	//	SendChunk(tSS()<<it->second.sDirname << "~~~" << it->first << " : " << it->second.fpr.size<<"<br>");

	LoadHints();
	UpdateIndexFiles();

	if(CheckAndReportError() || CheckStopSignal())
		goto save_fail_count;

	m_damageList.open(SZABSPATH(FNAME_DAMAGED), ios::out | ios::trunc);

	SendChunk("<b>Validating cache contents...</b><br>\n");
	ProcessSeenIndexFiles(*this);

	if(CheckAndReportError() || CheckStopSignal())
		goto save_fail_count;

	// update timestamps of pending removals
	LoadPreviousData(false);

	RemoveAndStoreStatus(cmd.find("purgeNow")!=stmiss);
	PurgeMaintLogs();

	DelTree(CACHE_BASE+"_actmp");

	PrintStats("Allocated disk space");

	SendChunk("<br>Done.</br>");

	save_fail_count:

	if (m_nErrorCount <= 0)
	{
		::unlink(FAIL_INI);
	}
	else
	{
		FILE *f = fopen(FAIL_INI, "a");
		if (!f)
		{
			SendFmt << "Unable to open " <<
			sFAIL_INI << " for writing, attempting to recreate... ";
			::unlink(FAIL_INI);
			f = ::fopen(FAIL_INI, "w");
			if (f)
				SendChunk("OK\n<br>\n");
			else
			{
				SendChunk(
						"<span class=\"ERROR\">FAILED. ABORTING. Check filesystem and file permissions.</span>");
			}
		}
		if (f)
		{
			::fprintf(f, "%lu\n", (long unsigned int) GetTime());
			checkForceFclose(f);
		}
	}

}

void expiration::PurgeMaintLogs()
{
	tStrDeq logs = ExpandFilePattern(acfg::logdir + SZPATHSEP MAINT_PFX "*.log*");
	if (logs.size() > 2)
		SendChunk(
				"Found required cleanup tasks: purging maintenance logs...<br>\n");
	for (tStrDeq::const_iterator it = logs.begin(); it != logs.end(); it++)
	{
		time_t id = atoofft(it->c_str() + acfg::logdir.size() + 7);
		//cerr << "id ist: "<< id<<endl;
		if (id == GetTaskId())
			continue;
		//cerr << "Remove: "<<globbuf.gl_pathv[i]<<endl;
#ifdef ENABLED
		::unlink(it->c_str());
#endif
	}
}

bool expiration::ProcessRegular(const string & sPathAbs, const struct stat &stinfo)
{

	if(CheckStopSignal())
		return false;

	if(sPathAbs.size()<=CACHE_BASE_LEN) // heh?
		return false;

	ProgTell();

	string sPathRel(sPathAbs, CACHE_BASE_LEN);
	DBGQLOG(sPathRel);

	// detect strings which are only useful for shell or js attacks
	if(sPathRel.find_first_of("\r\n'\"<>")!=stmiss)
		return true;

	if(sPathRel[0] == '_' && !m_bScanInternals)
		return true; // not for us

	// special handling for the installer files, we need an index for them which might be not there
	tStrPos pos2, pos = sPathRel.rfind("/installer-");
	if(pos!=stmiss && stmiss !=(pos2=sPathRel.find("/images/", pos)))
	{
		AddIFileCandidate(sPathRel.substr(0, pos2+8)+"MD5SUMS");
#warning teste das mit echtem image file
#if 0
		auto idir = sPathRel.substr(0, pos2 + 8);
		/* XXX: support sha256, do onl,y MD5SUMS for now
		 if(!ContHas(m_indexFilesRel, idir+"SHA256SUMS"))
		 {
		 */
		// folder doesn't have sha256 version but that's ok. At least md5 version is there.
		// XXX: change that when "oldstable" also has sha256 version
		auto& idesc = m_indexFilesRel[idir + "MD5SUMS"];
		/* pretend that it's there but not usable so the refreshing code will try to get at
		 * least one copy for that location if it's needed there
		 */
		idesc.vfile_ondisk = true;
		idesc.uptodate = false;
//		}
#endif
	}
	UINT stripLen=0;
    if (endsWithSzAr(sPathRel, ".head"))
		stripLen=5;
	else if (AddIFileCandidate(sPathRel))
	{
		auto &attr = SetFlags(sPathRel);
		attr.space += stinfo.st_size;
		attr.forgiveDlErrors = endsWith(sPathRel, sslIndex);
	}
	// ok, split to dir/file and add to the list
	tStrPos nCutPos = sPathRel.rfind(CPATHSEP);
	nCutPos = (nCutPos == stmiss) ? 0 : nCutPos+1;

	auto& finfo = m_trashFile2dir2Info[sPathRel.substr(nCutPos, sPathRel.length() - stripLen - nCutPos)]
	                                   [sPathRel.substr(0, nCutPos)];
	finfo.nLostAt = m_gMaintTimeNow;
	// remember the size for content data, ignore for the header file
	finfo.fpr.size = stripLen ? stinfo.st_size : 0;

	return true;
}

void expiration::LoadHints()
{
	filereader reader;
	if(!reader.OpenFile(acfg::confdir+SZPATHSEP+"ignore_list"))
	{
		if(acfg::suppdir.empty() || !reader.OpenFile(acfg::suppdir+SZPATHSEP+"ignore_list"))
				return;
	}
	string sTmp;
	while (reader.GetOneLine(sTmp))
	{
		trimLine(sTmp);
		if (startsWithSz(sTmp, "#"))
			continue;
		if(startsWith(sTmp, CACHE_BASE))
			sTmp.erase(CACHE_BASE_LEN);
		if(sTmp.empty())
			continue;
		SetFlags(sTmp).forgiveDlErrors=true;
	}

	reader.Close();
	reader.OpenFile(sFAIL_INI);
	while(reader.GetOneLine(sTmp))
		m_nPrevFailCount += (atol(sTmp.c_str())>0);



}
void expiration::LoadPreviousData(bool bForceInsert)
{
	filereader reader;
	reader.OpenFile(SABSPATH(FNAME_PENDING));

	string sLine;

	while(reader.GetOneLine(sLine))
	{
		char *eptr(NULL);
		const char *s = sLine.c_str();
		time_t timestamp = strtoull(s, &eptr, 10);
		if (!eptr || *eptr != '\t' || !timestamp || timestamp > m_gMaintTimeNow) // where is the DeLorean?
			continue;
		const char *sep = strchr(++eptr, (unsigned) '\t');
		if (!sep)
			continue;
		string dir(eptr, sep - eptr);
		if (!dir.empty() && '/' != *(sep - 1))
			dir += "/";
		const char *term = strchr(++sep, (unsigned) '\t'); // just to be sure
		if (term)
			continue;

		auto& desc = m_trashFile2dir2Info[sep][dir];
		// maybe add with timestamp from the last century (implies removal later)
		if(bForceInsert)
			desc.nLostAt=1;
		// considered file was already considered garbage, use the old date
		else if(desc.nLostAt>0)
			desc.nLostAt = timestamp;
	}

#if 0
	/*
	cout << "Unreferenced: ";
	for(trashmap_t::iterator it=m_trashCandidates.begin(); it!=m_trashCandidates.end(); it++)
		fprintf(stdout, "%lu\t%s\t%s\n",  it->second.first, it->second.second.c_str(), it->first.c_str());
	*/

	if(m_trashCandidates.size()>0)
	{
		// pretty printing for impatient users
		char buf[200];

		// map to to wait
		int nMax=acfg::extreshhold-((now-oldest)/86400);
		int nMin=acfg::extreshhold-((now-newest)/86400);

		snprintf(buf, _countof(buf), "Previously detected: %lu rotten package file(s), "
				"to be deleted in about %d-%d day(s)<br>\n",
				(unsigned long) m_trashCandidates.size(),
				nMin, nMax==nMin?nMax+1:nMax); // cheat a bit for the sake of code simplicity
		SendChunk(buf);
	}
#endif

}

inline bool expiration::CheckAndReportError()
{
	if (m_nErrorCount > 0 && m_bErrAbort)
	{
		SendChunk("<span class=\"ERROR\">Found errors during processing, "
				"aborting as requested.</span>");
		SendChunk(m_nPrevFailCount+(m_nErrorCount>0) > acfg::exsupcount
				? "<!-- TELL:THE:ADMIN -->"
						: "<!-- NOT:TELLING:THE:ADMIN:YET -->");
		return true;
	}
	return false;
}

