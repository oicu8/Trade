// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "utiltime.h"

#include <atomic>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread.hpp>

static std::atomic<int64_t> nMockTime(0); //!< For unit testing

int64_t GetTime()
{
    if (nMockTime) return nMockTime;

    return time(NULL);
}

void SetMockTime(int64_t nMockTimeIn)
{
    nMockTime.store(nMockTimeIn, std::memory_order_relaxed);
}

int64_t GetMockTime()
{
    return nMockTime.load(std::memory_order_relaxed);
}

int64_t GetTimeMillis()
{
    int64_t now = (boost::posix_time::microsec_clock::universal_time() -
                   boost::posix_time::ptime(boost::gregorian::date(1970,1,1))).total_milliseconds();
    assert(now > 0);
    return now;
}

int64_t GetTimeMicros()
{
    int64_t now = (boost::posix_time::microsec_clock::universal_time() -
                   boost::posix_time::ptime(boost::gregorian::date(1970,1,1))).total_microseconds();
    assert(now > 0);
    return now;
}

int64_t GetSystemTimeInSeconds()
{
    return GetTimeMicros()/1000000;
}

void MilliSleep(int64_t n)
{
    /*
     *  Not implementing HAVE_WORKING_BOOST_SLEEP_FOR from Bitcoin until we upgrade to a more modern
     *  build system which can easily support it
     */
/**
 * Boost's sleep_for was uninterruptible when backed by nanosleep from 1.50
 * until fixed in 1.52. Use the deprecated sleep method for the broken case.
 * See: https://svn.boost.org/trac/boost/ticket/7238
 */
/*
#if defined(HAVE_WORKING_BOOST_SLEEP_FOR)
    boost::this_thread::sleep_for(boost::chrono::milliseconds(n));
#elif defined(HAVE_WORKING_BOOST_SLEEP)
    boost::this_thread::sleep(boost::posix_time::milliseconds(n));
#else
//should never get here
#error missing boost sleep implementation
#endif
}
*/
#if BOOST_VERSION >= 105000
    boost::this_thread::sleep_for(boost::chrono::milliseconds(n));
#else
    boost::this_thread::sleep(boost::posix_time::milliseconds(n));
#endif
}

std::string DateTimeStrFormat(const char* pszFormat, int64_t nTime)
{
    static std::locale classic(std::locale::classic());
    // std::locale takes ownership of the pointer
    std::locale loc(classic, new boost::posix_time::time_facet(pszFormat));
    std::stringstream ss;
    ss.imbue(loc);
    ss << boost::posix_time::from_time_t(nTime);
    return ss.str();
}

/* TODO: update the code to remove this convenience method */
static const std::string strTimestampFormat = "%Y-%m-%d %H:%M:%S UTC";
std::string DateTimeStrFormat(int64_t nTime)
{
    return DateTimeStrFormat(strTimestampFormat.c_str(), nTime);
}

#ifdef WIN32
void _win32getlocaltime(tm *ptm, int *pms)
{
    // win native api to get local time
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    // convert
    ptm->tm_sec  = (int)st.wSecond;
    ptm->tm_min  = (int)st.wMinute;
    ptm->tm_hour = (int)st.wHour;
    ptm->tm_mday = (int)st.wDay;
    ptm->tm_mon  = (int)st.wMonth - 1;
    ptm->tm_year = (int)st.wYear - 1900;
    ptm->tm_wday = (int)st.wDayOfWeek;
    *pms         = st.wMilliseconds;
}
#endif
