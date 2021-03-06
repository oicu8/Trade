// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "alert.h"
#include "backtrace.h"
#include "checkpoints.h"
#include "db.h"
#include "txdb.h"
#include "net.h"
#include "init.h"
#include "ui_interface.h"
#include "kernel.h"
#include "robinhood.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include "darksend.h"
#include "masternode.h"
#include "spork.h"
#include "wallet.h"

#include <stdio.h>
#include <sstream>
#include <thread>

using namespace std;
using namespace boost;

extern std::atomic<bool> fRequestShutdown;
CCriticalSection cs_setpwalletRegistered;
std::set<CWallet*> setpwalletRegistered;
CCriticalSection cs_main;
CTxMemPool mempool;
unsigned int nTransactionsUpdated = 0;
robin_hood::unordered_node_map<uint256, CBlockIndex *> mapBlockIndex;
std::set<pair<COutPoint, unsigned int> > setStakeSeen;

CBigNum bnProofOfWorkLimit(~uint256(0) >> 20); // Starting Difficulty: results with 0,000244140625 proof-of-work difficulty
CBigNum bnProofOfWorkLimitTestNet(~uint256(0) >> 2);

static const int64_t nTargetTimespan = 20 * 60;  // Neutron - every 20mins
unsigned int nTargetSpacing = 1 * 79; // Neutron - 79 secs
static const int64_t nDiffChangeTarget = 1;
unsigned int nStakeMinAge = 5 * 60 * 60; // Neutron - 5 hours
unsigned int nStakeMaxAge = 5 * 60 * 60; // Neutron - 5 hours
unsigned int nModifierInterval = 10 * 60; // Neutron - time to elapse before new modifier is computed

int nCoinbaseMaturity = 80;
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
uint256 nBestChainTrust = 0;
uint256 nBestInvalidTrust = 0;
uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
int64_t nTimeBestReceived = 0;

#define ENFORCE_MN_PAYMENT_HEIGHT  1100000
#define ENFORCE_DEV_PAYMENT_HEIGHT 1200000

bool fEnforceMnWinner = false;

CMedianFilter<int> cPeerBlockCounts(5, 0); // Amount of blocks that other nodes claim to have
robin_hood::unordered_node_map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;
set<pair<COutPoint, unsigned int> > setStakeSeenOrphan;
map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "Neutron Signed Message:\n";

// Settings
int64_t nTransactionFee = MIN_TX_FEE;
int64_t nReserveBalance = 0;
int64_t nMinimumInputValue = 0;

extern enum Checkpoints::CPMode CheckpointsMode;

void RegisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.erase(pwalletIn);
    }
}

// Get the wallet transaction with the given hash (if it exists)
bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

// Make sure all wallets know about the given transaction, in the given block
void SyncWithWallets(const CTransaction& tx, const CBlock* pblock, bool fUpdate, bool fConnect)
{
    if (!fConnect)
    {
        // ppcoin: wallets need to refund inputs when disconnecting coinstake
        if (tx.IsCoinStake())
        {
            BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
            {
                if (pwallet->IsFromMe(tx))
                    pwallet->DisableTransaction(tx);
            }
        }

        return;
    }

    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(tx, pblock, fUpdate);
}

// Notify wallets about a new best chain
void static SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

// Notify wallets about an updated transaction
void static UpdatedTransaction(const uint256& hashTx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}

// Dump all wallets
void static PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
}

// Notify wallets about an incoming inventory (for request counts)
void static Inventory(const uint256& hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->Inventory(hash);
}

// Ask wallets to resend their transactions
void ResendWalletTransactions(bool fForce)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->ResendWalletTransactions(fForce);
}

bool AbortNode(const std::string &strMessage, const std::string &userMessage) {
    strMiscWarning = strMessage;

    LogPrintf("[FATAL] %s\n", strMessage.c_str());
    uiInterface.ThreadSafeMessageBox(userMessage.empty() ? _("Error: A fatal internal error occured, "
                                     "see debug.log for details") : userMessage, "",
                                     CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();

    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:

    size_t nSize = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);

    if (nSize > 5000)
    {
        LogPrintf("%s : ignoring large orphan tx (size: %u, hash: %s)\n", __func__,
                  nSize, hash.ToString().substr(0,10).c_str());

        return false;
    }

    mapOrphanTransactions[hash] = tx;

    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrintf("%s : stored orphan tx %s (mapsz %u)\n", __func__,
              hash.ToString().substr(0,10).c_str(), mapOrphanTransactions.size());

    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;

    const CTransaction& tx = mapOrphanTransactions[hash];

    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);

        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }

    mapOrphanTransactions.erase(hash);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;

    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan
        uint256 randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);

        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();

        EraseOrphanTx(it->first);
        ++nEvicted;
    }

    return nEvicted;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{
    SetNull();

    if (!txdb.ReadTxIndex(prevout.hash, txindexRet))
        return false;

    if (!ReadFromDisk(txindexRet.pos))
        return false;

    if (prevout.n >= vout.size())
    {
        SetNull();
        return false;
    }

    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::IsStandard() const
{
    if (nVersion > CTransaction::CURRENT_VERSION)
        return false;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
        // pay-to-script-hash, which is 3 ~80-byte signatures, 3
        // ~65-byte public keys, plus a few script ops.
        if (txin.scriptSig.size() > 500)
            return false;

        if (!txin.scriptSig.IsPushOnly())
            return false;

        if (fEnforceCanonical && !txin.scriptSig.HasCanonicalPushes())
            return false;
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;

    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        if (!::IsStandard(txout.scriptPubKey, whichType))
            return false;

        if (whichType == TX_NULL_DATA)
            nDataOut++;

        if (txout.nValue == 0)
            return false;

        if (fEnforceCanonical && !txout.scriptPubKey.HasCanonicalPushes())
            return false;
    }

    // Only one OP_RETURN txout is permitted
    if (nDataOut > 1)
        return false;

    return true;
}

// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1

bool CTransaction::AreInputsStandard(const MapPrevTx& mapInputs) const
{
    if (IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prev = GetOutputFor(vin[i], mapInputs);
        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;

        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;

        if (!Solver(prevScript, whichType, vSolutions))
            return false;

        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);

        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig the
        // IsStandard() call returns false
        vector<vector<unsigned char> > stack;

        if (!EvalScript(stack, vin[i].scriptSig, *this, i, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;

            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;

            if (!Solver(subscript, whichType2, vSolutions2))
                return false;

            if (whichType2 == TX_SCRIPTHASH)
                return false;

            int tmpExpected;
            tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);

            if (tmpExpected < 0)
                return false;

            nArgsExpected += tmpExpected;
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int
CTransaction::GetLegacySigOpCount() const
{
    unsigned int nSigOps = 0;

    BOOST_FOREACH(const CTxIn& txin, vin)
        nSigOps += txin.scriptSig.GetSigOpCount(false);

    BOOST_FOREACH(const CTxOut& txout, vout)
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);

    return nSigOps;
}


bool CTransaction::CheckTransaction() const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
        return DoS(10, error("%s : vin empty", __func__));

    if (vout.empty())
        return DoS(10, error("%s : vout empty", __func__));

    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("%s : size limits failed", __func__));

    // Check for negative or overflow output values
    int64_t nValueOut = 0;

    for (unsigned int i = 0; i < vout.size(); i++)
    {
        const CTxOut& txout = vout[i];

        if (txout.IsEmpty() && !IsCoinBase() && !IsCoinStake())
            return DoS(100, error("%s : txout empty for user transaction", __func__));

        if (txout.nValue < 0)
            return DoS(100, error("%s : txout.nValue negative", __func__));

        if (txout.nValue > MAX_MONEY)
            return DoS(100, error("%s : txout.nValue too high", __func__));

        nValueOut += txout.nValue;

        if (!MoneyRange(nValueOut))
            return DoS(100, error("%s : txout total out of range", __func__));
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return false;

        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (!fTestNet && (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100))
            return DoS(100, error("%s : coinbase script size is invalid", __func__));
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
            if (txin.prevout.IsNull())
                return DoS(10, error("%s : prevout is null", __func__));
        }
    }

    return true;
}

int64_t CTransaction::GetMinFee(unsigned int nBlockSize, enum GetMinFee_mode mode, unsigned int nBytes) const
{
    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE
    int64_t nBaseFee = (mode == GMF_RELAY) ? MIN_RELAY_TX_FEE : MIN_TX_FEE;
    unsigned int nNewBlockSize = nBlockSize + nBytes;
    int64_t nMinFee = (1 + (int64_t)nBytes / 1000) * nBaseFee;

    // To limit dust spam, require MIN_TX_FEE/MIN_RELAY_TX_FEE if any output is less than 0.01
    if (nMinFee < nBaseFee)
    {
        BOOST_FOREACH(const CTxOut& txout, vout)
        {
            if (txout.nValue < CENT)
                nMinFee = nBaseFee;
        }
    }

    // Raise the price as the block approaches full
    if (nBlockSize != 1 && nNewBlockSize >= MAX_BLOCK_SIZE_GEN / 2)
    {
        if (nNewBlockSize >= MAX_BLOCK_SIZE_GEN)
            return MAX_MONEY;

        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;

    return nMinFee;
}

bool AcceptableInputs(CTxMemPool& pool, const CTransaction &txo, bool fLimitFree,
                      bool* pfMissingInputs)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    CTransaction tx(txo);

    if (!tx.CheckTransaction())
        return error("%s : CheckTransaction() failed", __func__);

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("%s : coinbase as individual tx", __func__));

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return tx.DoS(100, error("%s : coinstake as individual tx", __func__));

    // Rather not work on nonstandard transactions (unless -testnet)
    string reason;

    if (!fTestNet && !tx.IsStandard())
        return error("%s : nonstandard transaction", __func__);

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();

    if (pool.exists(hash))
        return false;

    // Check for conflicts with in-memory transactions
    {
        LOCK(pool.cs); // protect pool.mapNextTx

        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            COutPoint outpoint = tx.vin[i].prevout;
            if (pool.mapNextTx.count(outpoint))
            {
                // Disable replacement feature for now
                return false;
            }
        }
    }

    {
        CTxDB txdb("r");

        // do we already have it?
        if (txdb.ContainsTx(hash))
            return false;

        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;

        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("%s : FetchInputs() found invalid tx %s", __func__, hash.ToString().c_str());

            if (pfMissingInputs)
                *pfMissingInputs = true;

            return false;
        }

        int64_t nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        int64_t txMinFee = tx.GetMinFee(1000, GMF_RELAY, nSize);

        if ((fLimitFree && nFees < txMinFee) || (!fLimitFree && nFees < MIN_TX_FEE))
        {
            return error("%s : not enough fees %s, %ld < %ld", __func__,
                         hash.ToString().c_str(), nFees, txMinFee);
        }

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < MIN_RELAY_TX_FEE)
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;

            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount > GetArg("-limitfreerelay", 15) * 10 * 1000)
                return error("%s : free transaction rejected by rate limiter", __func__);

            LogPrintf("%s : rate limit dFreeCount: %g => %g\n", __func__, dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        CDiskTxPos posThisTx(1,1,1);
        CBlockIndex* pindexBlock = pindexBest;

        if (!tx.IsCoinBase())
        {
            int64_t nValueIn = 0;
            int64_t nFees = 0;

            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint prevout = tx.vin[i].prevout;
                assert(mapInputs.count(prevout.hash) > 0);
                CTxIndex& txindex = mapInputs[prevout.hash].first;
                CTransaction& txPrev = mapInputs[prevout.hash].second;

                if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
                {
                    return error("%s : %s prevout.n out of range %d %u %u prev tx %s\n%s", __func__,
                                 tx.GetHash().ToString().substr(0,10).c_str(), prevout.n,
                                 txPrev.vout.size(), txindex.vSpent.size(),
                                 prevout.hash.ToString().substr(0,10).c_str(),
                                 txPrev.ToString().c_str());
                }

                // If prev is coinbase or coinstake, check that it's matured
                if (txPrev.IsCoinBase() || txPrev.IsCoinStake())
                {
                    for (const CBlockIndex* pindex = pindexBlock; pindex &&
                         pindexBlock->nHeight - pindex->nHeight < nCoinbaseMaturity;
                         pindex = pindex->pprev)
                    {
                        if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
                        {
                            return error("%s : tried to spend %s at depth %d", __func__,
                                         txPrev.IsCoinBase() ? "coinbase" : "coinstake",
                                         pindexBlock->nHeight - pindex->nHeight);
                        }
                    }
                }

                // ppcoin: check transaction timestamp
                if (txPrev.nTime > tx.nTime)
                    return error("%s : transaction timestamp earlier than input transaction", __func__);

                // Check for negative or overflow input values
                nValueIn += txPrev.vout[prevout.n].nValue;

                if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                    return error("%s : txin values out of range", __func__);

            }

            // The first loop above does all the inexpensive checks.
            // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
            // Helps prevent CPU exhaustion attacks.
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint prevout = tx.vin[i].prevout;
                assert(mapInputs.count(prevout.hash) > 0);
                CTxIndex& txindex = mapInputs[prevout.hash].first;

                // Check for conflicts (double-spend)
                // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
                // for an attacker to attempt to split the network.
                if (!txindex.vSpent[prevout.n].IsNull())
                {
                    return error("%s : %s prev tx already used at %s", __func__,
                                 tx.GetHash().ToString().substr(0,10).c_str(),
                                 txindex.vSpent[prevout.n].ToString().c_str());
                }

                // Mark outpoints as spent
                txindex.vSpent[prevout.n] = posThisTx;
            }

            if (!tx.IsCoinStake())
            {
                if (nValueIn < tx.GetValueOut())
                {
                    return error("%s : %s value in < value out", __func__,
                                 tx.GetHash().ToString().substr(0,10).c_str());
                }

                // Tally transaction fees
                int64_t nTxFee = nValueIn - tx.GetValueOut();

                if (nTxFee < 0)
                {
                    return error("%s : %s nTxFee < 0", __func__,
                                 tx.GetHash().ToString().substr(0,10).c_str());
                }

                // enforce transaction fees for every block
                if (nTxFee < tx.GetMinFee())
                {
                    return error("%s : %s not paying required fee=%s, paid=%s", __func__,
                                 tx.GetHash().ToString().substr(0,10).c_str(),
                                 FormatMoney(tx.GetMinFee()).c_str(),
                                 FormatMoney(nTxFee).c_str());
                }

                nFees += nTxFee;

                if (!MoneyRange(nFees))
                    return error("%s : nFees out of range", __func__);
            }
        }
    }

    return true;
}

bool CTransaction::AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs, bool* pfMissingInputs)
{
    return mempool.accept(txdb, *this, fCheckInputs, pfMissingInputs);
}

int GetInputAge(CTxIn& vin)
{
    const uint256& prevHash = vin.prevout.hash;
    CTransaction tx;
    uint256 hashBlock;
    bool fFound = GetTransaction(prevHash, tx, hashBlock);

    if(fFound)
    {
        if(mapBlockIndex.find(hashBlock) != mapBlockIndex.end())
            return pindexBest->nHeight - mapBlockIndex[hashBlock]->nHeight;
        else
            return 0;
    }
    else
        return 0;
}

bool CWalletTx::AcceptWalletTransaction(CTxDB& txdb, bool fCheckInputs)
{
    {
        LOCK(mempool.cs);

        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!(tx.IsCoinBase() || tx.IsCoinStake()))
            {
                uint256 hash = tx.GetHash();

                if (!mempool.exists(hash) && !txdb.ContainsTx(hash))
                    tx.AcceptToMemoryPool(txdb, fCheckInputs);
            }
        }

        return AcceptToMemoryPool(txdb, fCheckInputs);
    }

    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    CTxDB txdb("r");
    return AcceptWalletTransaction(txdb);
}

int CTxIndex::GetDepthInMainChain() const
{
    // Read block header
    CBlock block;

    if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))
        return 0;

    // Find the block in the index
    auto mi = mapBlockIndex.find(block.GetHash());

    if (mi == mapBlockIndex.end())
        return 0;

    CBlockIndex* pindex = (*mi).second;

    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return 1 + nBestHeight - pindex->nHeight;
}

// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock)
{
    {
        LOCK(cs_main);

        {
            LOCK(mempool.cs);

            if (mempool.exists(hash))
            {
                tx = mempool.lookup(hash);
                return true;
            }
        }

        CTxDB txdb("r");
        CTxIndex txindex;

        if (tx.ReadFromDisk(txdb, COutPoint(hash, 0), txindex))
        {
            CBlock block;

            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();

            return true;
        }
    }

    return false;
}

CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;

    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;

    while (pblockindex->pprev && pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;

    while (pblockindex->pnext && pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;

    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions)
{
    if (!fReadTransactions)
    {
        *this = pindex->GetBlockHeader();
        return true;
    }

    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, fReadTransactions))
        return false;

    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");

    return true;
}

uint256 static GetOrphanRoot(const CBlock* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];

    return pblock->GetHash();
}

// ppcoin: find block wanted by given orphan block
uint256 WantedByOrphan(const CBlock* pblockOrphan)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblockOrphan->hashPrevBlock))
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrevBlock];

    return pblockOrphan->hashPrevBlock;
}

// Miners coin base reward
int64_t GetProofOfWorkReward(int64_t nFees, int nHeight)
{
    if(fTestNet)
    {
        if (nHeight == 1)
            return 50000000 * COIN;

        return 5000 * COIN;
    }

    // Anti-instamine
    int64_t nSubsidy = 0 * COIN;

    if(nHeight < 120)
        nSubsidy = 0 * COIN;
    else if(nHeight < 950)
        nSubsidy = 750 * COIN;
    else if(nHeight < 1400)
        nSubsidy = 550 * COIN;
    else if(nHeight < 1900)
        nSubsidy = 425 * COIN;
    else if(nHeight < 2400)
        nSubsidy =  325 * COIN;
    else if(nHeight < 2850)
        nSubsidy = 251 * COIN;
    else if(nHeight < 3500)
        nSubsidy = 190 * COIN;
    else if(nHeight < 4000)
        nSubsidy = 105 * COIN;

    return nSubsidy + nFees;
}

// Declare halving period for pos
static const int g_RewardHalvingPeriod = 1000000;

// Miners coin stake reward based on coin age spent (coin-days)
int64_t GetProofOfStakeReward(int64_t nCoinAge, int64_t nFees, int nHeight)
{
    int64_t nSubsidy = 40 * COIN;

    if(nHeight < 5000)
        nSubsidy = 30 * COIN;
    else if(nHeight < 7000)
        nSubsidy = 45 * COIN;
    else if(nHeight < 7250)
        nSubsidy = 190 * COIN;
    else if(nHeight < 8500)
        nSubsidy = 80 * COIN;
    else if(nHeight < 10000)
        nSubsidy = 15 * COIN;
    else if(nHeight < 13500)
        nSubsidy = 30 * COIN;
    else
    {
        nSubsidy = 40 * COIN;

        // Subsidy is cut in half every g_RewardHalvingPeriod blocks which will occur approximately every 2 years.
        int halvings = nHeight / g_RewardHalvingPeriod;

        nSubsidy = (halvings >= 64)? 0 : (nSubsidy >> halvings);
        nSubsidy -= nSubsidy*(nHeight % g_RewardHalvingPeriod)/(2*g_RewardHalvingPeriod);
    }

    return nSubsidy + nFees;
}


// Maximum nBits value could possible be required nTime after
unsigned int ComputeMaxBits(CBigNum bnTargetLimit, unsigned int nBase, int64_t nTime)
{
    CBigNum bnResult;
    bnResult.SetCompact(nBase);
    bnResult *= 2;

    while (nTime > 0 && bnResult < bnTargetLimit)
    {
        // Maximum 200% adjustment per day...
        bnResult *= 2;
        nTime -= 24 * 60 * 60;
    }

    if (bnResult > bnTargetLimit)
        bnResult = bnTargetLimit;

    return bnResult.GetCompact();
}

// Minimum amount of stake that could possibly be required nTime after
// minimum proof-of-stake required was nBase
unsigned int ComputeMinStake(int height, unsigned int nBase, int64_t nTime, unsigned int nBlockTime)
{
    return ComputeMaxBits(GetPOSLimit(height), nBase, nTime);
}

// ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;

    return pindex;
}

// Minimum amount of work that could possibly be required nTime after
// minimum work required was nBase
unsigned int ComputeMinWork(unsigned int nBase, int64_t nTime)
{
    return ComputeMaxBits(bnProofOfWorkLimit, nBase, nTime);
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, bool fProofOfStake)
{
    CBigNum bnTargetLimit = fProofOfStake ? GetPOSLimit(pindexLast->nHeight) : bnProofOfWorkLimit;

    if (pindexLast == NULL)
        return bnTargetLimit.GetCompact(); // Genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);

    if (pindexPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // First block

    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);

    if (pindexPrevPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // Second block

    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

    if (nActualSpacing < 0)
        nActualSpacing = nTargetSpacing;

    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    CBigNum bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    int64_t nInterval = nTargetTimespan / nTargetSpacing;

    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}


bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit)
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->nChainTrust > nBestInvalidTrust)
    {
        nBestInvalidTrust = pindexNew->nChainTrust;
        CTxDB().WriteBestInvalidTrust(CBigNum(nBestInvalidTrust));
        uiInterface.NotifyBlocksChanged();
    }

    uint256 nBestInvalidBlockTrust = pindexNew->nChainTrust - pindexNew->pprev->nChainTrust;
    uint256 nBestBlockTrust = pindexBest->nHeight != 0 ? (pindexBest->nChainTrust -
                                                          pindexBest->pprev->nChainTrust) :
                                                          pindexBest->nChainTrust;

    LogPrintf("%s : invalid block=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n", __func__,
              pindexNew->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->nHeight,
              CBigNum(pindexNew->nChainTrust).ToString().c_str(), nBestInvalidBlockTrust.Get64(),
              DateTimeStrFormat("%x %H:%M:%S", pindexNew->GetBlockTime()).c_str());

    LogPrintf("%s : current best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n", __func__,
              hashBestChain.ToString().substr(0,20).c_str(), nBestHeight,
              CBigNum(pindexBest->nChainTrust).ToString().c_str(),
              nBestBlockTrust.Get64(), DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());
}

void CBlock::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(GetBlockTime(), GetAdjustedTime());
}

bool CTransaction::DisconnectInputs(CTxDB& txdb)
{
    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
            COutPoint prevout = txin.prevout;

            // Get prev txindex from disk
            CTxIndex txindex;

            if (!txdb.ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            if (prevout.n >= txindex.vSpent.size())
                return error("DisconnectInputs() : prevout.n out of range");

            // Mark outpoint as not spent
            txindex.vSpent[prevout.n].SetNull();

            // Write back
            if (!txdb.UpdateTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : UpdateTxIndex failed");
        }
    }

    // Remove transaction from index
    // This can fail if a duplicate of this transaction was in a chain that got
    // reorganized away. This is only possible if this transaction was completely
    // spent, so erasing it would be a no-op anyway.
    txdb.EraseTxIndex(*this);

    return true;
}

bool CTransaction::FetchInputs(CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool,
                               bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid)
{
    // FetchInputs can return false either because we just haven't seen some inputs
    // (in which case the transaction should be stored as an orphan)
    // or because the transaction is malformed (in which case the transaction should
    // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
    fInvalid = false;

    if (IsCoinBase())
        return true; // Coinbase transactions have no inputs to fetch.

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        COutPoint prevout = vin[i].prevout;

        if (inputsRet.count(prevout.hash))
            continue; // Got it already

        // Read txindex
        CTxIndex& txindex = inputsRet[prevout.hash].first;
        bool fFound = true;

        if ((fBlock || fMiner) && mapTestPool.count(prevout.hash))
        {
            // Get txindex from current proposed changes
            txindex = mapTestPool.find(prevout.hash)->second;
        }
        else
        {
            // Read txindex from txdb
            fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        }

        if (!fFound && (fBlock || fMiner))
        {
            return fMiner ? false : error("FetchInputs() : %s prev tx %s index entry not found",
                                          GetHash().ToString().substr(0,10).c_str(),
                                          prevout.hash.ToString().substr(0,10).c_str());
        }

        // Read txPrev
        CTransaction& txPrev = inputsRet[prevout.hash].second;

        if (!fFound || txindex.pos == CDiskTxPos(1,1,1))
        {
            // Get prev tx from single transactions in memory
            {
                LOCK(mempool.cs);

                if (!mempool.exists(prevout.hash))
                {
                    return error("FetchInputs() : %s mempool Tx prev not found %s",
                                 GetHash().ToString().substr(0,10).c_str(),
                                 prevout.hash.ToString().substr(0,10).c_str());
                }

                txPrev = mempool.lookup(prevout.hash);
            }

            if (!fFound)
                txindex.vSpent.resize(txPrev.vout.size());
        }
        else
        {
            // Get prev tx from disk
            if (!txPrev.ReadFromDisk(txindex.pos))
            {
                return error("FetchInputs() : %s ReadFromDisk prev tx %s failed",
                             GetHash().ToString().substr(0,10).c_str(),
                             prevout.hash.ToString().substr(0,10).c_str());
            }
        }
    }

    // Make sure all prevout.n indexes are valid:
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const COutPoint prevout = vin[i].prevout;
        assert(inputsRet.count(prevout.hash) != 0);

        const CTxIndex& txindex = inputsRet[prevout.hash].first;
        const CTransaction& txPrev = inputsRet[prevout.hash].second;

        if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
        {
            // Revisit this if/when transaction replacement is implemented and allows adding inputs
            fInvalid = true;

            return DoS(100, error("FetchInputs() : %s prevout.n out of range %d %u %u prev tx %s\n%s",
                                  GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(),
                                  txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(),
                                  txPrev.ToString().c_str()));
        }
    }

    return true;
}

const CTxOut& CTransaction::GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const
{
    MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);

    if (mi == inputs.end())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");

    const CTransaction& txPrev = (mi->second).second;

    if (input.prevout.n >= txPrev.vout.size())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");

    return txPrev.vout[input.prevout.n];
}

int64_t CTransaction::GetValueIn(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    int64_t nResult = 0;

    for (unsigned int i = 0; i < vin.size(); i++)
        nResult += GetOutputFor(vin[i], inputs).nValue;

    return nResult;
}

unsigned int CTransaction::GetP2SHSigOpCount(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prevout = GetOutputFor(vin[i], inputs);

        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(vin[i].scriptSig);
    }

    return nSigOps;
}

bool CTransaction::ConnectInputs(CTxDB& txdb, MapPrevTx inputs, map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
                                 const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, bool *txAlreadyUsed)
{
    // Take over previous transactions' spent pointers
    // fBlock is true when this is called from AcceptBlock when a new best-block is added to the blockchain
    // fMiner is true when called from the internal bitcoin miner
    // ... both are false when called from CTransaction::AcceptToMemoryPool
    if (!IsCoinBase())
    {
        int64_t nValueIn = 0;
        int64_t nFees = 0;

        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
            {
                return DoS(100, error("%s : %s prevout.n out of range %d %u %u prev tx %s\n%s", __func__,
                                      GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(),
                                      txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(),
                                      txPrev.ToString().c_str()));
            }

            // If prev is coinbase or coinstake, check that it's matured
            if (txPrev.IsCoinBase() || txPrev.IsCoinStake())
            {
                for (const CBlockIndex* pindex = pindexBlock;
                     pindex && pindexBlock->nHeight - pindex->nHeight < nCoinbaseMaturity; pindex = pindex->pprev)
                {
                    if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
                    {
                        return error("%s : tried to spend %s at depth %d", __func__,
                                     txPrev.IsCoinBase() ? "coinbase" : "coinstake",
                                     pindexBlock->nHeight - pindex->nHeight);
                    }
                }
            }

            // ppcoin: check transaction timestamp
            if (txPrev.nTime > nTime)
                return DoS(100, error("%s : transaction timestamp earlier than input transaction", __func__));

            // Check for negative or overflow input values
            nValueIn += txPrev.vout[prevout.n].nValue;

            if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return DoS(100, error("%s : txin values out of range", __func__));

        }

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            // Check for conflicts (double-spend)
            // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
            // for an attacker to attempt to split the network.
            if (!txindex.vSpent[prevout.n].IsNull())
            {
                if (txAlreadyUsed != nullptr)
                    *txAlreadyUsed = true;

                return fMiner ? false : error("%s : %s prev tx already used at %s", __func__,
                                              GetHash().ToString().substr(0,10).c_str(),
                                              txindex.vSpent[prevout.n].ToString().c_str());
            }

            // Skip ECDSA signature verification when connecting blocks (fBlock=true)
            // before the last blockchain checkpoint. This is safe because block merkle hashes are
            // still computed and checked, and any change will be caught at the next checkpoint.
            if (!(fBlock && (nBestHeight < Checkpoints::GetTotalBlocksEstimate())))
            {
                // Verify signature
                if (!VerifySignature(txPrev, *this, i, 0))
                    return DoS(100,error("%s : %s VerifySignature failed", __func__, GetHash().ToString().substr(0,10).c_str()));
            }

            // Mark outpoints as spent
            txindex.vSpent[prevout.n] = posThisTx;

            // Write back
            if (fBlock || fMiner)
            {
                mapTestPool[prevout.hash] = txindex;
            }
        }

        if (!IsCoinStake())
        {
            if (nValueIn < GetValueOut())
                return DoS(100, error("%s : %s value in < value out", __func__, GetHash().ToString().substr(0,10).c_str()));

            // Tally transaction fees
            int64_t nTxFee = nValueIn - GetValueOut();

            if (nTxFee < 0)
                return DoS(100, error("%s : %s nTxFee < 0", __func__, GetHash().ToString().substr(0,10).c_str()));

            // enforce transaction fees for every block
            if (nTxFee < GetMinFee())
            {
                return fBlock? DoS(100, error("%s : %s not paying required fee=%s, paid=%s", __func__,
                                              GetHash().ToString().substr(0,10).c_str(), FormatMoney(GetMinFee()).c_str(),
                                              FormatMoney(nTxFee).c_str())) : false;
            }

            nFees += nTxFee;

            if (!MoneyRange(nFees))
                return DoS(100, error("%s : nFees out of range", __func__));
        }
    }

    return true;
}


bool CTransaction::ClientConnectInputs()
{
    if (IsCoinBase())
        return false;

    // Take over previous transactions' spent pointers
    {
        LOCK(mempool.cs);
        int64_t nValueIn = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            // Get prev tx from single transactions in memory
            COutPoint prevout = vin[i].prevout;
            if (!mempool.exists(prevout.hash))
                return false;
            CTransaction& txPrev = mempool.lookup(prevout.hash);

            if (prevout.n >= txPrev.vout.size())
                return false;

            // Verify signature
            if (!VerifySignature(txPrev, *this, i, 0))
                return error("%s : VerifySignature failed", __func__);

            ///// this is redundant with the mempool.mapNextTx stuff,
            ///// not sure which I want to get rid of
            ///// this has to go away now that posNext is gone
            // // Check for conflicts
            // if (!txPrev.vout[prevout.n].posNext.IsNull())
            //     return error("ConnectInputs() : prev tx already used");
            //
            // // Flag outpoints as used
            // txPrev.vout[prevout.n].posNext = posThisTx;

            nValueIn += txPrev.vout[prevout.n].nValue;

            if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return error("%s : txin values out of range", __func__);
        }
        if (GetValueOut() > nValueIn)
            return false;
    }

    return true;
}

bool CBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{
    // Disconnect in reverse order
    for (int i = vtx.size() - 1; i >= 0; i--)
        if (!vtx[i].DisconnectInputs(txdb))
            return false;

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("%s : WriteBlockIndex failed", __func__);
    }

    // ppcoin: clean up wallet after disconnecting coinstake
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, false, false);

    return true;
}

bool CBlock::CalculateBlockAmounts(CTxDB& txdb, CBlockIndex *pindex, std::map<uint256, CTxIndex>& mapQueuedChanges,
                                   int64_t& nFees, int64_t& nValueIn, int64_t& nValueOut, int64_t& nStakeReward,
                                   bool fJustCheck, bool skipTxCheck, bool connectInputs)
{
    unsigned int nSigOps = 0;
    unsigned int nTxPos;

    if (fJustCheck)
    {
        // FetchInputs treats CDiskTxPos(1,1,1) as a special "refer to memorypool" indicator
        // Since we're just checking the block and not actually connecting it,
        // it might not (and probably shouldn't) be on the disk to get the transaction from
        nTxPos = 1;
    }
    else
    {
        nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                 (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(vtx.size());
    }

    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();

        // Do not allow blocks that contain transactions which 'overwrite' older transactions,
        // unless those are already completely spent.
        // If such overwrites are allowed, coinbases and transactions depending upon those
        // can be duplicated to remove the ability to spend the first instance -- even after
        // being sent to another address.
        // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
        // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
        // already refuses previously-known transaction ids entirely.
        // This rule was originally applied all blocks whose timestamp was after March 15, 2012, 0:00 UTC.
        // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
        // two in the chain that violate it. This prevents exploiting the issue against nodes in their
        // initial block download.
        CTxIndex txindexOld;

        if (!skipTxCheck && txdb.ReadTxIndex(hashTx, txindexOld))
        {
            BOOST_FOREACH(CDiskTxPos &pos, txindexOld.vSpent)
                if (pos.IsNull())
                    return DoS(50, error("%s : tried to overwrite transaction(s)", __func__));
        }

        nSigOps += tx.GetLegacySigOpCount();

        if (nSigOps > MAX_BLOCK_SIGOPS)
            return DoS(100, error("%s : too many sigops", __func__));

        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);

        if (!fJustCheck)
            nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);

        MapPrevTx mapInputs;

        if (tx.IsCoinBase())
            nValueOut += tx.GetValueOut();
        else
        {
            bool fInvalid;

            if (!tx.FetchInputs(txdb, mapQueuedChanges, true, false, mapInputs, fInvalid))
            {
                LogPrintf("%s : fetchinputs failed\n", __func__);
                return false;
            }

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += tx.GetP2SHSigOpCount(mapInputs);

            if (nSigOps > MAX_BLOCK_SIGOPS)
                return DoS(100, error("%s : too many sigops", __func__));

            int64_t nTxValueIn = tx.GetValueIn(mapInputs);
            int64_t nTxValueOut = tx.GetValueOut();

            nValueIn += nTxValueIn;
            nValueOut += nTxValueOut;

            if (!tx.IsCoinStake())
                nFees += nTxValueIn - nTxValueOut;
            if (tx.IsCoinStake())
                nStakeReward = nTxValueOut - nTxValueIn;

            bool txAlreadyUsed = false;

            if (connectInputs && !tx.ConnectInputs(txdb, mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, &txAlreadyUsed))
            {
                if (skipTxCheck && txAlreadyUsed)
                {
                    if (fDebug)
                        LogPrintf("%s : Skipping, did not connect previously connected inputs\n", __func__);
                }
                else
                {
                    LogPrintf("%s : failed to connect inputs\n", __func__);
                    return false;
                }
            }
        }

        mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());
    }

    return true;
}

bool CBlock::ConnectBlock(CTxDB& txdb, CBlockIndex *pindex, bool fJustCheck, bool reorganize, int postponedBlocks)
{
    // Check it again in case a previous version let a bad block in, but skip BlockSig checking
    if (!CheckBlock(!fJustCheck, !fJustCheck, false))
    {
        LogPrintf("%s : block check failed\n", __func__);
        return false;
    }

    map<uint256, CTxIndex> mapQueuedChanges;
    int64_t nFees = 0;
    int64_t nValueIn = 0;
    int64_t nValueOut = 0;
    int64_t nStakeReward = 0;

    if (!CalculateBlockAmounts(txdb, pindex, mapQueuedChanges, nFees, nValueIn, nValueOut, nStakeReward,
                               fJustCheck, reorganize, true))
    {
        LogPrintf("%s : Block transaction scan and amount calculations failed\n", __func__);
        return false;
    }

    // ppcoin: track money supply and mint amount info
    pindex->nMint = nValueOut - nValueIn + nFees;
    pindex->nMoneySupply = pindex->pprev ? pindex->pprev->nMoneySupply : 0;

    if (pindex->nMoneySupply == 0)
    {
        LogPrintf("%s : pprev address: %x\n", __func__, pindex->pprev);

        if (pindex->pprev)
        {
            LogPrintf("%s : pprev->pprev address: %x\n", __func__, pindex->pprev->pprev);
            LogPrintf("%s : pprev->nMoneySupply: %s\n", __func__, FormatMoney(pindex->pprev->nMoneySupply));

            if (pindex->pprev->pprev)
                LogPrintf("%: pprev->pprev->nMoneySupply: %s\n", __func__, FormatMoney(pindex->pprev->pprev->nMoneySupply));
        }

        Backtrace::output();

        if (!IsInitialBlockDownload())
            return error("%s : Money supply was calculated to zero\n", __func__);
    }

    pindex->nMoneySupply = pindex->nMoneySupply + nValueOut - nValueIn;
    fEnforceMnWinner = sporkManager.IsSporkActive(SPORK_2_MASTERNODE_WINNER_ENFORCEMENT);

    if (fDebug && fEnforceMnWinner)
        LogPrintf("%s : specific masternode winner enforcement enabled\n", __func__);

    if (IsProofOfWork())
    {
        int64_t nReward = GetProofOfWorkReward(nFees, pindex->nHeight);

        // Check coinbase reward after hardcoded checkpoint
        if (pindex->nHeight > 17901 && vtx[0].GetValueOut() > nReward)
        {
            return DoS(50, error("%s : coinbase reward exceeded (actual=%d vs calculated=%d)",
                                 __func__, vtx[0].GetValueOut(), nReward));
        }
    }
    else if (IsProofOfStake())
    {
        // ppcoin: coin stake tx earns reward instead of paying fee
        uint64_t nCoinAge;

        if (!vtx[1].GetCoinAge(txdb, nCoinAge))
        {
            return error("%s : %s unable to get coin age for coinstake", __func__,
                         vtx[1].GetHash().ToString().substr(0,10).c_str());
        }

        int64_t nCalculatedStakeReward = GetProofOfStakeReward(nCoinAge, nFees, pindex->nHeight);

        if (pindex->nHeight > 17901 && nStakeReward > nCalculatedStakeReward)
        {
            return DoS(100, error("%s : coinstake pays too much(actual=%d vs calculated=%d)", __func__,
                                  nStakeReward, nCalculatedStakeReward));
        }

        // Check block rewards
        if (!IsInitialBlockDownload())
        {
            if (isMasternodeListSynced)
            {
                masternodePayments.ProcessBlock(pindex->nHeight + 1);
                masternodePayments.ProcessBlock(pindex->nHeight + 2);
                masternodePayments.ProcessBlock(pindex->nHeight + 3);
            }

            CAmount nRequiredMnPmt = GetMasternodePayment(pindex->nHeight, nCalculatedStakeReward);
            int64_t nRequiredDevPmt = GetDeveloperPayment(nCalculatedStakeReward);
            int64_t nRequiredStakePmt = nCalculatedStakeReward - nRequiredMnPmt - nRequiredDevPmt;

            LogPrintf("\n%s : *Block %d reward=%s - Expected payouts: Stake=%s, Masternode=%s, Project=%s\n",
                      __func__, pindex->nHeight, FormatMoney(nCalculatedStakeReward),
                      FormatMoney(nRequiredStakePmt), FormatMoney(nRequiredMnPmt),
                      FormatMoney(nRequiredDevPmt));

            int nDoS_PMTs = sporkManager.GetSporkValue(SPORK_4_PAYMENT_ENFORCEMENT_DOS_VALUE);

            CScript expectedPayee;
            CScript blockPayee;
            bool fMnPaymentMade = false;
            bool fPaidCorrectMn = false;
            bool fValidMnPayment = false;

            for (const CTxOut out : vtx[1].vout)
            {
                if(out.nValue == nRequiredMnPmt)
                {
                    fMnPaymentMade = true;
                    blockPayee = out.scriptPubKey;
                }
            }

            // case: expected masternode amount incorrect/none
            if (!fMnPaymentMade)
            {
                if (pindex->nHeight >= ENFORCE_MN_PAYMENT_HEIGHT)
                    return DoS(nDoS_PMTs, error("%s : Stake does not pay masternode expected amount", __func__));
                else
                    LogPrintf("%s : Stake does not pay masternode expected amount\n", __func__);
            }

            // check payee once masternode list obtained
            if (isMasternodeListSynced && MNPAYEE_MAX_BLOCK_AGE > GetTime() - pindex->GetBlockTime())
            {
                if (masternodePayments.GetBlockPayee(pindex->nHeight, expectedPayee))
                {
                    if (blockPayee == expectedPayee)
                        fPaidCorrectMn = true;
                    else
                    {
                        /* if the current block payment is invalid it might just be a matter of the
                           payment list being out of sync... */
                        LogPrintf("%s : Possible discrepancy found in masternode payment, recalculating payee...\n", __func__);

                        masternodePayments.ProcessBlock(pindex->nHeight, reorganize);
                        masternodePayments.GetBlockPayee(pindex->nHeight, expectedPayee);
                        fPaidCorrectMn = blockPayee == expectedPayee;
                    }

                    CTxDestination paidDest;
                    bool hasBlockPayee = ExtractDestination(blockPayee, paidDest);
                    CBitcoinAddress paidMN(paidDest);

                    // case: expected masternode address not paid
                    if (!fPaidCorrectMn)
                    {
                        CTxDestination expectDest;
                        bool fPrintAddress = ExtractDestination(expectedPayee, expectDest);
                        CBitcoinAddress addressMN(expectDest);

                        if (fEnforceMnWinner && postponedBlocks < sporkManager.GetSporkValue(SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD))
                        {
                            Backtrace::output();

                            return DoS(nDoS_PMTs, error("%s : Stake does not pay correct masternode, "
                                       "actual=%s required=%s, block=%d, postponedBlocks=%d\n",
                                       __func__, hasBlockPayee ? paidMN.ToString() : "",
                                       fPrintAddress ? addressMN.ToString() : "", pindex->nHeight, postponedBlocks));
                        }
                        else
                        {
                            LogPrintf("%s : Stake does not pay correct masternode, actual=%s required=%s, block=%d, "
                                      "postponedBlocks=%d\n", __func__, hasBlockPayee ? paidMN.ToString() : "",
                                      fPrintAddress ? addressMN.ToString() : "", pindex->nHeight, postponedBlocks);
                        }
                    }
                    else
                    {
                        LogPrintf("%s : Stake pays correct masternode, address=%s, block=%d\n", __func__,
                                  hasBlockPayee ? paidMN.ToString() : "", pindex->nHeight);
                    }
                }
                else
                {
                    // case: was not able to determine correct masternode payee
                    LogPrintf("%s : Did not have expected masternode payee for block %d\n", __func__, pindex->nHeight);
                }

                // verify correct payment addr and amount
                fValidMnPayment = fMnPaymentMade && fPaidCorrectMn;

                if (!fValidMnPayment && postponedBlocks < sporkManager.GetSporkValue(SPORK_12_PAYMENT_ENFORCEMENT_THRESHOLD))
                {
                    if (fEnforceMnWinner)
                        return DoS(nDoS_PMTs, error("%s : Masternode payment missing or is not valid", __func__));
                    else
                        LogPrintf("%s : Masternode payment missing or is not valid\n", __func__);
                }
            }
            else
            {
                LogPrintf("%s : Masternode list not yet synced or block too old "
                          " (CountEnabled=%d)\n",__func__, mnodeman.CountEnabled());
            }

            // check developer payment
            bool fValidDevPmt = false;
            CScript scriptDev = GetDeveloperScript();

            // check coinstake tx for dev payment
            for (const CTxOut out : vtx[1].vout)
            {
                if (out.nValue == nRequiredDevPmt && out.scriptPubKey == scriptDev)
                    fValidDevPmt = true;
            }

            if (!fValidDevPmt)
            {
                if (pindex->nHeight >= ENFORCE_DEV_PAYMENT_HEIGHT)
                {
                    return DoS(nDoS_PMTs, error("%s : Block fails to pay dev payment of %s\n", __func__,
                                                FormatMoney(nRequiredDevPmt).c_str()));
                }
                else
                {
                    LogPrintf("%s : Block does not pay %s dev payment - NOT ENFORCED\n", __func__,
                              FormatMoney(nRequiredDevPmt).c_str());
                }
            }

            if (fDebug)
                LogPrintf("ConnectBlock() : Stake pays dev payment\n");

        }
        else
        {
            masternodePayments.AddPastWinningMasternode(vtx,
                    GetMasternodePayment(pindex->nHeight, nCalculatedStakeReward), pindex->nHeight);

            LogPrintf("%s : Initial block download: skipping masternode and developer payment checks %d\n", __func__,
                      pindex->nHeight);
        }
    }

    if (!txdb.WriteBlockIndex(CDiskBlockIndex(pindex)))
        return error("%s : WriteBlockIndex for pindex failed", __func__);

    if (fJustCheck)
        return true;

    // Write queued txindex changes
    for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
    {
        if (!txdb.UpdateTxIndex((*mi).first, (*mi).second))
            return error("%s : UpdateTxIndex failed", __func__);
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();

        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("%s : WriteBlockIndex failed", __func__);
    }

    // Watch for transactions paying to me
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, true);

    return true;
}

bool static Reorganize(CTxDB& txdb, CBlockIndex* pindexNew, int postponedBlocks)
{
    LogPrintf("[%s]\n", __func__);

    CBlockIndex* pfork = pindexBest;
    CBlockIndex* plonger = pindexNew;

    while (pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight)
        {
            if (!(plonger = plonger->pprev))
                return error("%s : plonger->pprev is null", __func__);
        }

        if (pfork == plonger)
            break;

        if (!(pfork = pfork->pprev))
            return error("%s : pfork->pprev is null", __func__);
    }

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;

    for (CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect
    vector<CBlockIndex*> vConnect;

    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);

    reverse(vConnect.begin(), vConnect.end());

    LogPrintf("%s : Disconnect %u blocks; %s..%s\n", __func__, vDisconnect.size(),
              pfork->GetBlockHash().ToString().substr(0,20).c_str(),
              pindexBest->GetBlockHash().ToString().substr(0,20).c_str());

    LogPrintf("%s : Connect %u blocks; %s..%s\n", __func__, vConnect.size(),
              pfork->GetBlockHash().ToString().substr(0,20).c_str(),
              pindexNew->GetBlockHash().ToString().substr(0,20).c_str());

    // Disconnect shorter branch
    vector<CTransaction> vResurrect;

    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
        CBlock block;

        if (!block.ReadFromDisk(pindex))
            return error("%s : ReadFromDisk for disconnect failed", __func__);

        if (!block.DisconnectBlock(txdb, pindex))
        {
            return error("%s : DisconnectBlock %s failed", __func__,
                         pindex->GetBlockHash().ToString().substr(0,20).c_str());
        }

        // Queue memory transactions to resurrect
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
        {
            if (!(tx.IsCoinBase() || tx.IsCoinStake()))
                vResurrect.push_back(tx);
        }
    }

    // Connect longer branch
    vector<CTransaction> vDelete;

    for (unsigned int i = 0; i < vConnect.size(); i++)
    {
        CBlockIndex* pindex = vConnect[i];
        CBlock block;

        if (!block.ReadFromDisk(pindex))
            return error("%s : ReadFromDisk for connect failed", __func__);

        if (!block.ConnectBlock(txdb, pindex, false, true, postponedBlocks))
        {
            // Invalid block
            return error("%s : ConnectBlock %s failed", __func__, pindex->GetBlockHash().ToString().substr(0,20).c_str());
        }

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);
    }

    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("%s : WriteHashBestChain failed", __func__);

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("%s : TxnCommit failed", __func__);

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;
    }

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
    {
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;
    }

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        tx.AcceptToMemoryPool(txdb, false);

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete)
    {
        mempool.remove(tx);
        mempool.removeConflicts(tx);
    }

    LogPrintf("[%s] : Done\n", __func__);
    return true;
}


// Called from inside SetBestChain: attaches a block to the new best chain being built
bool CBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew, bool reorganize, int postponedBlocks)
{
    uint256 hash = GetHash();

    // Adding to current best branch
    if (!ConnectBlock(txdb, pindexNew, reorganize, postponedBlocks) || !txdb.WriteHashBestChain(hash))
    {
        txdb.TxnAbort();
        InvalidChainFound(pindexNew);

        return false;
    }

    if (!txdb.TxnCommit())
        return error("%s : TxnCommit failed", __func__);

    // Add to current best branch
    if (pindexNew->pprev != NULL)
        pindexNew->pprev->pnext = pindexNew;

    // Delete redundant memory transactions
    BOOST_FOREACH(CTransaction& tx, vtx)
        mempool.remove(tx);

    return true;
}

bool CBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
    uint256 hash = GetHash();

    if (!txdb.TxnBegin())
        return error("%s : TxnBegin failed", __func__);

    if (pindexGenesisBlock == NULL && hash == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))
    {
        txdb.WriteHashBestChain(hash);

        if (!txdb.TxnCommit())
            return error("%s : TxnCommit failed", __func__);

        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {
        if (!SetBestChainInner(txdb, pindexNew, false))
            return error("%s : SetBestChainInner failed", __func__);
    }
    else
    {
        // the first block in the new chain that will cause it to become the new best chain
        CBlockIndex *pindexIntermediate = pindexNew;

        // list of blocks that need to be connected afterwards
        std::vector<CBlockIndex*> vpindexSecondary;

        // Reorganize is costly in terms of db load, as it works in a single db transaction.
        // Try to limit how much needs to be done inside
        while (pindexIntermediate->pprev && pindexIntermediate->pprev->nChainTrust > pindexBest->nChainTrust)
        {
            vpindexSecondary.push_back(pindexIntermediate);
            pindexIntermediate = pindexIntermediate->pprev;
        }

        if (!vpindexSecondary.empty())
            LogPrintf("%s : Postponing %u reconnects\n", __func__, vpindexSecondary.size());

        LogPrintf("%s : The tail of the new chain is at block %d\n", __func__, pindexNew->nHeight);

        int postponedBlocks = vpindexSecondary.empty() ? -1 : vpindexSecondary.size();

        // Switch to new best branch
        if (!Reorganize(txdb, pindexIntermediate, postponedBlocks))
        {
            txdb.TxnAbort();
            InvalidChainFound(pindexNew);

            return error("%s : Reorganize failed", __func__);
        }

        // Connect further blocks
        BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
        {
            CBlock block;

            if (!block.ReadFromDisk(pindex))
            {
                LogPrintf("%s : ReadFromDisk failed\n", __func__);
                break;
            }

            if (!txdb.TxnBegin())
            {
                LogPrintf("%s : TxnBegin 2 failed\n", __func__);
                break;
            }

            // Errors now are not fatal, we still did a reorganisation to a new chain in a valid way
            if (!block.SetBestChainInner(txdb, pindex, true, postponedBlocks))
            {
                pindexNew = pindex->pprev;
                break;
            }
        }
    }

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = IsInitialBlockDownload();

    if (!fIsInitialDownload)
    {
        const CBlockLocator locator(pindexNew);
        ::SetBestChain(locator);
    }

    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    pindexBest->pnext = nullptr; /* Should already be null or with pnext being invalid - effectively disconnectng the rest */
    nBestHeight = pindexBest->nHeight;
    nBestChainTrust = pindexNew->nChainTrust;
    nTimeBestReceived = GetTime();
    nTransactionsUpdated++;

    uint256 nBestBlockTrust = pindexBest->nHeight != 0 ? (pindexBest->nChainTrust -
                              pindexBest->pprev->nChainTrust) : pindexBest->nChainTrust;

    LogPrintf("%s : new best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n", __func__,
              hashBestChain.ToString().substr(0,20).c_str(), nBestHeight,
              CBigNum(nBestChainTrust).ToString().c_str(),
              nBestBlockTrust.Get64(), DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;

        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;

            pindex = pindex->pprev;
        }

        if (nUpgraded > 0)
        {
            LogPrintf("%s : %d of last 100 blocks above version %d\n", __func__,
                      nUpgraded, (int)CBlock::CURRENT_VERSION);
        }

        if (nUpgraded > 100 / 2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (fDebug)
        LogPrintf("%s : Blocknotify string is \"%s\"\n", __func__, strCmd);

    if (!fIsInitialDownload && !strCmd.empty())
    {
        if (fDebug)
            LogPrintf("%s : Starting blocknotify thread and command\n", __func__);

        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        std::thread t(runCommand, strCmd);
        t.detach(); // Thread runs free
    }

    return true;
}

// ppcoin: total coin age spent in transaction, in the unit of coin-days.
// Only those coins meeting minimum age requirement counts. As those
// transactions not in main chain are not currently indexed so we
// might not find out about their coin age. Older transactions are
// guaranteed to be in main chain by sync-checkpoint. This rule is
// introduced to help nodes establish a consistent view of the coin
// age (trust score) of competing branches.

bool CTransaction::GetCoinAge(CTxDB& txdb, uint64_t& nCoinAge) const
{
    CBigNum bnCentSecond = 0;  // coin age in the unit of cent-seconds
    nCoinAge = 0;

    if (IsCoinBase())
        return true;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // First try finding the previous transaction in database
        CTransaction txPrev;
        CTxIndex txindex;

        if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
            continue;  // Previous transaction not in main chain

        if (nTime < txPrev.nTime)
            return false;  // Transaction timestamp violation

        // Read block header
        CBlock block;

        if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
            return false; // unable to read block of previous transaction

        if (block.GetBlockTime() + nStakeMinAge > nTime)
            continue; // only count coins meeting min age requirement

        int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
        bnCentSecond += CBigNum(nValueIn) * (nTime-txPrev.nTime) / CENT;

        if (fDebug && GetBoolArg("-printcoinage"))
        {
            LogPrintf("coin age nValueIn=%d nTimeDiff=%d bnCentSecond=%s\n",
                      nValueIn, nTime - txPrev.nTime, bnCentSecond.ToString().c_str());
        }
    }

    CBigNum bnCoinDay = bnCentSecond * CENT / COIN / (24 * 60 * 60);

    if (fDebug && GetBoolArg("-printcoinage"))
        LogPrintf("coin age bnCoinDay=%s\n", bnCoinDay.ToString().c_str());

    nCoinAge = bnCoinDay.getuint64();
    return true;
}

// ppcoin: total coin age spent in block, in the unit of coin-days.
bool CBlock::GetCoinAge(uint64_t& nCoinAge) const
{
    nCoinAge = 0;
    CTxDB txdb("r");

    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uint64_t nTxCoinAge;

        if (tx.GetCoinAge(txdb, nTxCoinAge))
            nCoinAge += nTxCoinAge;
        else
            return false;
    }

    if (nCoinAge == 0) // Block coin age minimum 1 coin-day
        nCoinAge = 1;

    if (fDebug && GetBoolArg("-printcoinage"))
        LogPrintf("block coin age total nCoinDays=%d\n", nCoinAge);

    return true;
}

bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos, const uint256& hashProof)
{
    // Check for duplicate
    uint256 hash = GetHash();

    if (mapBlockIndex.count(hash))
        return error("%s : %s already exists", __func__, hash.ToString());

    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);

    if (!pindexNew)
        return error("%s : CBlockIndex allocation failed", __func__);

    pindexNew->phashBlock = &hash;
    auto miPrev = mapBlockIndex.find(hashPrevBlock);

    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }

    // ppcoin: compute chain trust score
    pindexNew->nChainTrust = (pindexNew->pprev ? pindexNew->pprev->nChainTrust : 0) + pindexNew->GetBlockTrust();

    // ppcoin: compute stake entropy bit for stake modifier
    if (!pindexNew->SetStakeEntropyBit(GetStakeEntropyBit()))
        return error("%s : SetStakeEntropyBit failed", __func__);

    // Record proof hash value
    pindexNew->hashProof = hashProof;

    // ppcoin: compute stake modifier
    uint64_t nStakeModifier = 0;
    bool fGeneratedStakeModifier = false;

    if (!ComputeNextStakeModifier(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
        return error("%s : ComputeNextStakeModifier failed", __func__);

    pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
    pindexNew->nStakeModifierChecksum = GetStakeModifierChecksum(pindexNew);

    // if (!CheckStakeModifierCheckpoints(pindexNew->nHeight, pindexNew->nStakeModifierChecksum))
    //     return error("AddToBlockIndex() : Rejected by stake modifier checkpoint height=%d, modifier=0x%016"
    //                  PRIx64, pindexNew->nHeight, nStakeModifier);

    auto mi = mapBlockIndex.emplace(hash, pindexNew).first;

    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

    pindexNew->phashBlock = &((*mi).first);

    // Write to disk block index
    CTxDB txdb;

    if (!txdb.TxnBegin())
        return false;

    txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));

    if (!txdb.TxnCommit())
        return false;

    // New best
    if (pindexNew->nChainTrust > nBestChainTrust)
    {
        if (!SetBestChain(txdb, pindexNew))
            return false;
    }

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = vtx[0].GetHash();
    }

    uiInterface.NotifyBlocksChanged();
    return true;
}

bool CBlock::CheckBlock(bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig) const
{
    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("%s : size limits failed", __func__));

    // Check proof of work matches claimed amount
    // if (fCheckPOW && IsProofOfWork() && !CheckProofOfWork(GetPoWHash(), nBits))
    //    return DoS(50, error("CheckBlock() : proof of work failed"));

    // Check timestamp
    if (GetBlockTime() > FutureDrift(GetAdjustedTime()))
        return error("%s : block timestamp too far in the future", __func__);

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return DoS(100, error("%s : first tx is not coinbase", __func__));

    for (unsigned int i = 1; i < vtx.size(); i++)
    {
        if (vtx[i].IsCoinBase())
            return DoS(100, error("%s : more than one coinbase", __func__));
    }

    // Check coinbase timestamp
    if (GetBlockTime() > FutureDrift((int64_t)vtx[0].nTime))
        return DoS(50, error("%s : coinbase timestamp is too early", __func__));

    if (IsProofOfStake())
    {
        // Coinbase output should be empty if proof-of-stake block
        if (vtx[0].vout.size() != 1 || !vtx[0].vout[0].IsEmpty())
            return DoS(100, error("%s : coinbase output not empty for proof-of-stake block", __func__));

        // Second transaction must be coinstake, the rest must not be
        if (vtx.empty() || !vtx[1].IsCoinStake())
            return DoS(100, error("%s : second tx is not coinstake", __func__));

        for (unsigned int i = 2; i < vtx.size(); i++)
        {
            if (vtx[i].IsCoinStake())
                return DoS(100, error("%s : more than one coinstake", __func__));
        }

        // Neutron: check proof-of-stake block signature
        if (fCheckSig && !CheckBlockSignature())
            return DoS(100, error("%s : bad proof-of-stake block signature", __func__));
    }

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        if (!tx.CheckTransaction())
            return DoS(tx.nDoS, error("%s : CheckTransaction failed", __func__));

        // ppcoin: check transaction timestamp
        if (GetBlockTime() < (int64_t)tx.nTime)
            return DoS(50, error("%s : block timestamp earlier than transaction timestamp", __func__));
    }

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;

    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }

    if (uniqueTx.size() != vtx.size())
        return DoS(100, error("%s : duplicate transaction", __func__));

    unsigned int nSigOps = 0;

    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += tx.GetLegacySigOpCount();
    }

    if (nSigOps > MAX_BLOCK_SIGOPS)
        return DoS(100, error("%s : out-of-bounds SigOpCount", __func__));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return DoS(100, error("%s : hashMerkleRoot mismatch", __func__));

    return true;
}

bool CBlock::AcceptBlock()
{
    if (fTestNet && nVersion > CURRENT_VERSION)
        return DoS(10, error("%s : reject unknown block version %d", __func__, nVersion));

    // Check for duplicate
    uint256 hash = GetHash();

    if (mapBlockIndex.count(hash))
        return error("%s : block already in mapBlockIndex", __func__);

    auto mi = mapBlockIndex.find(hashPrevBlock);

    if (mi == mapBlockIndex.end())
        return DoS(10, error("%s : prev block not found", __func__));

    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight + 1;

    if (IsProofOfWork() && nHeight > LAST_POW_BLOCK)
        return DoS(100, error("%s : reject proof-of-work at height %d", __func__, nHeight));

    // Check proof-of-work or proof-of-stake
    if (nBits != GetNextTargetRequired(pindexPrev, IsProofOfStake()))
    {
        return DoS(100, error("%s : incorrect %s", __func__,
                              IsProofOfWork() ? "proof-of-work" : "proof-of-stake"));
    }

    // Check timestamp against prev
    if (GetBlockTime() <= pindexPrev->GetPastTimeLimit() ||
        FutureDrift(GetBlockTime()) < pindexPrev->GetBlockTime())
    {
        return error("%s : block's timestamp is too early", __func__);
    }

    // Check coinstake timestamp
    if (IsProofOfStake() && !CheckCoinStakeTimestamp(nHeight, GetBlockTime(), (int64_t)vtx[1].nTime))
    {
        return DoS(50, error("%s : coinstake timestamp violation nTimeBlock=%d nTimeTx=%u",
                             __func__, GetBlockTime(), vtx[1].nTime));
    }

    // Check that all transactions are finalized
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        if (!tx.IsFinal(nHeight, GetBlockTime()))
            return DoS(10, error("%s : contains a non-final transaction", __func__));
    }

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckHardened(nHeight, hash))
        return DoS(100, error("%s : rejected by hardened checkpoint lock-in at %d", __func__, nHeight));

    uint256 hashProof;

    // Verify hash target and signature of coinstake tx
    if (IsProofOfStake())
    {
        uint256 targetProofOfStake;

        if (!CheckProofOfStake(pindexPrev, vtx[1], nBits, hashProof, targetProofOfStake))
        {
            LogPrintf("%s : [WARNING] check proof-of-stake failed for block %s\n", __func__, hash.ToString().c_str());

            if (!IsInitialBlockDownload())
                return false;
        }
    }

    // PoW is checked in CheckBlock()
    if (IsProofOfWork())
        hashProof = GetPoWHash();

    bool cpSatisfies = Checkpoints::CheckSync(hash, pindexPrev);

    // Check that the block satisfies synchronized checkpoint
    if (CheckpointsMode == Checkpoints::STRICT && !cpSatisfies)
        return error("%s : rejected by synchronized checkpoint", __func__);

    if (CheckpointsMode == Checkpoints::ADVISORY && !cpSatisfies)
        strMiscWarning = _("WARNING: syncronized checkpoint violation detected, but skipped!");

    // Enforce rule that the coinbase starts with serialized block height
    CScript expect = CScript() << nHeight;

    if (vtx[0].vin[0].scriptSig.size() < expect.size() ||
        !std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
        return DoS(100, error("%s : block height mismatch in coinbase", __func__));

    // Write block to history file
    if (!CheckDiskSpace(::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION)))
        return error("%s : out of disk space", __func__);

    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;

    if (!WriteToDisk(nFile, nBlockPos))
        return error("%s : WriteToDisk failed", __func__);

    if (!AddToBlockIndex(nFile, nBlockPos, hashProof))
        return error("%s : AddToBlockIndex failed", __func__);

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();

    if (hashBestChain == hash)
    {
        LOCK(cs_vNodes);

        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
        }
    }

    // ppcoin: check pending sync-checkpoint
    Checkpoints::AcceptPendingSyncCheckpoint();

    return true;
}

uint256 CBlockIndex::GetBlockTrust() const
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    if (bnTarget <= 0)
        return 0;

    return ((CBigNum(1) << 256) / (bnTarget + 1)).getuint256();
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;

    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;

        pstart = pstart->pprev;
    }

    return (nFound >= nRequired);
}

bool ProcessNewBlock(CNode* pfrom, CBlock* pblock)
{
    // Check for duplicate
    uint256 hash = pblock->GetHash();

    if (mapBlockIndex.count(hash))
    {
        return error("%s : already have block %d %s", __func__,
                     mapBlockIndex[hash]->nHeight, hash.ToString().substr(0,20).c_str());
    }

    if (mapOrphanBlocks.count(hash))
        return error("%s : already have block (orphan) %s", __func__, hash.ToString().substr(0,20).c_str());

    // ppcoin: check proof-of-stake
    // Limited duplicity on stake: prevents block flood attack
    // Duplicate stake allowed only when there is orphan child block
    if (!IsInitialBlockDownload() && pblock->IsProofOfStake() && setStakeSeen.count(pblock->GetProofOfStake()) &&
        !mapOrphanBlocksByPrev.count(hash) && !Checkpoints::WantedByPendingSyncCheckpoint(hash))
    {
        return error("%s : duplicate proof-of-stake (%s, %d) for block %s", __func__,
                     pblock->GetProofOfStake().first.ToString().c_str(),
                     pblock->GetProofOfStake().second, hash.ToString().c_str());
    }

    // Preliminary checks
    if (!pblock->CheckBlock())
        return error("%s : CheckBlock FAILED", __func__);

    CBlockIndex* pcheckpoint = Checkpoints::GetLastSyncCheckpoint();

    if (pcheckpoint && pblock->hashPrevBlock != hashBestChain &&
        !Checkpoints::WantedByPendingSyncCheckpoint(hash))
    {
        // Extra checks to prevent "fill up memory by spamming with bogus blocks"
        int64_t deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
        CBigNum bnNewBlock;
        bnNewBlock.SetCompact(pblock->nBits);
        CBigNum bnRequired;

        if (pblock->IsProofOfStake())
        {
            auto mi = mapBlockIndex.find(pblock->hashPrevBlock);
            int height = 0; // presume zero, relaxing the check if the height can't be determined

            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindexPrev = (*mi).second;
                height = pindexPrev->nHeight + 1;
            }

            bnRequired.SetCompact(ComputeMinStake(height, GetLastBlockIndex(pcheckpoint, true)->nBits,
                                                  deltaTime, pblock->nTime));
        }
        else
        {
            bnRequired.SetCompact(ComputeMinWork(GetLastBlockIndex(pcheckpoint, false)->nBits,
                                                                   deltaTime));
        }

        if (bnNewBlock > bnRequired)
        {
            std::stringstream msg;
            msg << boost::format("%s : block with too little %s") % __func__ %
                (pblock->IsProofOfStake() ? "proof-of-stake" : "proof-of-work");

            if (pfrom)
            {
                pfrom->Misbehaving(msg.str(), 100);
            }

            return error(msg.str().c_str());
        }
    }

    // ppcoin: ask for pending sync-checkpoint if any
    if (!IsInitialBlockDownload())
        Checkpoints::AskForPendingSyncCheckpoint(pfrom);

    // If don't already have its previous block, shunt it off to holding area until we get it
    if (!mapBlockIndex.count(pblock->hashPrevBlock))
    {
        if (fDebug)
        {
            LogPrintf("%s : Missing orphan block with hash %s\n", __func__,
                      pblock->hashPrevBlock.ToString().c_str());
        }

        CBlock* pblock2 = new CBlock(*pblock);

        // ppcoin: check proof-of-stake
        if (pblock2->IsProofOfStake())
        {
            // Limited duplicity on stake: prevents block flood attack
            // Duplicate stake allowed only when there is orphan child block
            if (setStakeSeenOrphan.count(pblock2->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash) &&
                !Checkpoints::WantedByPendingSyncCheckpoint(hash))
            {
                return error("%s : duplicate proof-of-stake (%s, %d) for orphan block %s", __func__,
                             pblock2->GetProofOfStake().first.ToString().c_str(),
                             pblock2->GetProofOfStake().second, hash.ToString().c_str());
            }
            else
                setStakeSeenOrphan.insert(pblock2->GetProofOfStake());
        }

        mapOrphanBlocks.emplace(hash, pblock2);
        mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

        // Ask this guy to fill in what we're missing
        if (pfrom)
        {
            if (fDebug)
            {
                LogPrintf("%s : Asking for missing blocks between index %d to hash %s\n", __func__,
                          pindexBest->nHeight, GetOrphanRoot(pblock2).ToString().c_str());
            }

            pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(pblock2));

            // ppcoin: getblocks may not obtain the ancestor block rejected
            // earlier by duplicate-stake check so we ask for it again directly
            if (!IsInitialBlockDownload())
                pfrom->AskFor(CInv(MSG_BLOCK, WantedByOrphan(pblock2)));
        }

        return true;
    }

    // Store to disk
    if (!pblock->AcceptBlock())
    {
        std::stringstream msg;
        msg << boost::format("%s : AcceptBlock for %s with parent %s FAILED") % __func__ %
            hash.ToString().c_str() % pblock->hashPrevBlock.ToString().c_str();

        if (pfrom)
            pfrom->Misbehaving(msg.str(), 5);

        return error(msg.str().c_str());
    }

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);

    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];

        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev); ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;

            if (pblockOrphan->AcceptBlock())
                vWorkQueue.push_back(pblockOrphan->GetHash());

            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            setStakeSeenOrphan.erase(pblockOrphan->GetProofOfStake());
            delete pblockOrphan;
        }

        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    // ppcoin: if responsible for sync-checkpoint send it
    if (pfrom && !CSyncCheckpoint::strMasterPrivKey.empty())
        Checkpoints::SendSyncCheckpoint(Checkpoints::AutoSelectSyncCheckpoint());

    if (fDebug)
        LogPrintf("%s: ACCEPTED\n", __func__);

    return true;
}

bool CBlock::SignBlock_POW(const CKeyStore& keystore)
{
    vector<valtype> vSolutions;
    txnouttype whichType;

    for(unsigned int i = 0; i < vtx[0].vout.size(); i++)
    {
        const CTxOut& txout = vtx[0].vout[i];

        if (!Solver(txout.scriptPubKey, whichType, vSolutions))
            continue;

        // Sign
        CKey key;
        valtype& vchPubKey = vSolutions[0];
        CScript scriptPubKey;

        if (whichType == TX_PUBKEY)
        {
            if (!keystore.GetKey(Hash160(vchPubKey), key))
                continue;

            if (key.GetPubKey() != vchPubKey)
                continue;

            hashMerkleRoot = BuildMerkleTree();

            if(!key.Sign(GetHash(), vchBlockSig))
                continue;

            return true;
        }

        if (whichType == TX_PUBKEYHASH) // pay to address type
        {
            // Convert to pay to public key type
            if (!keystore.GetKey(uint160(vSolutions[0]), key))
            {
                if (fDebug && GetBoolArg("-printcoinstake"))
                    LogPrintf("%s : failed to get key for kernel type=%d\n", __func__, whichType);

                continue;  // unable to find corresponding public key
            }

            if (key.GetPubKey() != vchPubKey)
                continue;

            hashMerkleRoot = BuildMerkleTree();

            if(!key.Sign(GetHash(), vchBlockSig))
                continue;

            return true;
        }
    }

    LogPrintf("Sign failed\n");
    return false;
}

bool CBlock::SignBlock(CWallet& wallet, int64_t nFees)
{
    // if we are trying to sign something except POS block template
    if (!vtx[0].vout[0].IsEmpty())
        return false;

    // if we are trying to sign a complete POS block
    if (IsProofOfStake())
        return true;

    static int64_t nLastCoinStakeSearchTime = GetAdjustedTime(); // startup timestamp

    CKey key;
    CTransaction txCoinStake;

    if (GetPOSProtocolVersion(nBestHeight + 1) == 2)
        txCoinStake.nTime &= ~STAKE_TIMESTAMP_MASK;

    int64_t nSearchTime = txCoinStake.nTime; // search to current time

    if (nSearchTime > nLastCoinStakeSearchTime)
    {
        if (wallet.CreateCoinStake(wallet, nBits, nSearchTime-nLastCoinStakeSearchTime, nFees, txCoinStake, key))
        {
            if (txCoinStake.nTime >= max(pindexBest->GetPastTimeLimit()+1, PastDrift(pindexBest->GetBlockTime())))
            {
                // make sure coinstake would meet timestamp protocol
                //    as it would be the same as the block timestamp
                vtx[0].nTime = nTime = txCoinStake.nTime;
                nTime = max(pindexBest->GetPastTimeLimit()+1, GetMaxTransactionTime());
                nTime = max(GetBlockTime(), PastDrift(pindexBest->GetBlockTime()));

                // we have to make sure that we have no future timestamps in
                //    our transactions set
                for (vector<CTransaction>::iterator it = vtx.begin(); it != vtx.end();)
                    if (it->nTime > nTime) { it = vtx.erase(it); } else { ++it; }

                vtx.insert(vtx.begin() + 1, txCoinStake);
                hashMerkleRoot = BuildMerkleTree();

                // append a signature to our block
                return key.Sign(GetHash(), vchBlockSig);
            }
        }
        nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
        nLastCoinStakeSearchTime = nSearchTime;
    }

    return false;
}

bool CBlock::CheckBlockSignature() const
{
    if (IsProofOfWork())
        return vchBlockSig.empty();

    vector<valtype> vSolutions;
    txnouttype whichType;

    const CTxOut& txout = vtx[1].vout[1];

    if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        valtype& vchPubKey = vSolutions[0];
        CKey key;
        if (!key.SetPubKey(vchPubKey))
            return false;
        if (vchBlockSig.empty())
            return false;
        return key.Verify(GetHash(), vchBlockSig);
    }

    return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
    {
        fShutdown = true;
        string strMessage = _("Warning: Disk space is low!");
        strMiscWarning = strMessage;
        LogPrintf("*** %s\n", strMessage.c_str());
        uiInterface.ThreadSafeMessageBox(strMessage, "Neutron", CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION | CClientUIInterface::MODAL);
        StartShutdown();
        return false;
    }
    return true;
}

bool LoadBlockIndex(bool fAllowNew)
{
    CBigNum bnTrustedModulus;

    if (fTestNet)
    {
        pchMessageStart[0] = 0xaf;
        pchMessageStart[1] = 0xf4;
        pchMessageStart[2] = 0xc1;
        pchMessageStart[3] = 0xa2;

        bnTrustedModulus.SetHex("f0d14cf72623dacfe738d0892b599be0f31052239cddd95a3f25101c801dc990453b38c9434efe3f372db39a32c2bb44cbaea72d62c8931fa785b0ec44531308df3e46069be5573e49bb29f4d479bfc3d162f57a5965db03810be7636da265bfced9c01a6b0296c77910ebdc8016f70174f0f18a57b3b971ac43a934c6aedbc5c866764a3622b5b7e3f9832b8b3f133c849dbcc0396588abcd1e41048555746e4823fb8aba5b3d23692c6857fccce733d6bb6ec1d5ea0afafecea14a0f6f798b6b27f77dc989c557795cc39a0940ef6bb29a7fc84135193a55bcfc2f01dd73efad1b69f45a55198bd0e6bef4d338e452f6a420f1ae2b1167b923f76633ab6e55");
        bnProofOfWorkLimit = bnProofOfWorkLimitTestNet; // 16 bits PoW target limit for testnet
        nStakeMinAge = 1 * 60 * 60; // test net min age is 1 hour
        nCoinbaseMaturity = 10; // test maturity is 10 blocks
        nModifierInterval = 6;
    }
    else
    {
        bnTrustedModulus.SetHex("d01f952e1090a5a72a3eda261083256596ccc192935ae1454c2bafd03b09e6ed11811be9f3a69f5783bbbced8c6a0c56621f42c2d19087416facf2f13cc7ed7159d1c5253119612b8449f0c7f54248e382d30ecab1928dbf075c5425dcaee1a819aa13550e0f3227b8c685b14e0eae094d65d8a610a6f49fff8145259d1187e4c6a472fa5868b2b67f957cb74b787f4311dbc13c97a2ca13acdb876ff506ebecbb904548c267d68868e07a32cd9ed461fbc2f920e9940e7788fed2e4817f274df5839c2196c80abe5c486df39795186d7bc86314ae1e8342f3c884b158b4b05b4302754bf351477d35370bad6639b2195d30006b77bf3dbb28b848fd9ecff5662bf39dde0c974e83af51b0d3d642d43834827b8c3b189065514636b8f2a59c42ba9b4fc4975d4827a5d89617a3873e4b377b4d559ad165748632bd928439cfbc5a8ef49bc2220e0b15fb0aa302367d5e99e379a961c1bc8cf89825da5525e3c8f14d7d8acca2fa9c133a2176ae69874d8b1d38b26b9c694e211018005a97b40848681b9dd38feb2de141626fb82591aad20dc629b2b6421cef1227809551a0e4e943ab99841939877f18f2d9c0addc93cf672e26b02ed94da3e6d329e8ac8f3736eebbf37bb1a21e5aadf04ee8e3b542f876aa88b2adf2608bd86329b7f7a56fd0dc1c40b48188731d11082aea360c62a0840c2db3dad7178fd7e359317ae081");
    }

    //
    // Load block index
    //
    CTxDB txdb("cr+");
    if (!txdb.LoadBlockIndex())
        return false;

    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
            return false;

        // Genesis block

        // MainNet:

        //CBlock(hash=000001faef25dec4fbcf906e6242621df2c183bf232f263d0ba5b101911e4563, ver=1, hashPrevBlock=0000000000000000000000000000000000000000000000000000000000000000, hashMerkleRoot=12630d16a97f24b287c8c2594dda5fb98c9e6c70fc61d44191931ea2aa08dc90, nTime=1393221600, nBits=1e0fffff, nNonce=164482, vtx=1, vchBlockSig=)
        //  Coinbase(hash=12630d16a9, nTime=1393221600, ver=1, vin.size=1, vout.size=1, nLockTime=0)
        //    CTxIn(COutPoint(0000000000, 4294967295), coinbase 00012a24323020466562203230313420426974636f696e2041544d7320636f6d6520746f20555341)
        //    CTxOut(empty)
        //  vMerkleTree: 12630d16a9
        //block.nTime = 1423862862
        //block.nNonce = 620091
        //block.nVersion = 1
        //block.hashMerkleRoot = da3215e78c191c4e5dd00e8ac2b57f71b20cdcad0c37562d39912df09a2f4d34
        //block.GetHash = 000009d2f828234d65299216e258242a4ea75d1b8d8a71d076377145068f08de
        // TestNet:

        //CBlock(hash=0000724595fb3b9609d441cbfb9577615c292abf07d996d3edabc48de843642d, ver=1, hashPrevBlock=0000000000000000000000000000000000000000000000000000000000000000, hashMerkleRoot=12630d16a97f24b287c8c2594dda5fb98c9e6c70fc61d44191931ea2aa08dc90, nTime=1393221600, nBits=1f00ffff, nNonce=216178, vtx=1, vchBlockSig=)
        //  Coinbase(hash=12630d16a9, nTime=1393221600, ver=1, vin.size=1, vout.size=1, nLockTime=0)
        //    CTxIn(COutPoint(0000000000, 4294967295), coinbase 00012a24323020466562203230313420426974636f696e2041544d7320636f6d6520746f20555341)
        //    CTxOut(empty)
        //  vMerkleTree: 12630d16a9

        const char* pszTimestamp = "April 18th 2015 Global stocks nosedive";
        std::vector<CTxIn> vin;
        vin.resize(1);
        vin[0].scriptSig = CScript() << 0 << CBigNum(42) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        std::vector<CTxOut> vout;
        vout.resize(1);
        vout[0].SetEmpty();
        CTransaction txNew(1, 1429352955, vin, vout, 0);
        CBlock block;
        block.vtx.push_back(txNew);
        block.hashPrevBlock = 0;
        block.hashMerkleRoot = block.BuildMerkleTree();
        block.nVersion = 1;
        block.nTime    = 1429352955;
        block.nBits    = (!fTestNet ? bnProofOfWorkLimit.GetCompact() : bnProofOfWorkLimitTestNet.GetCompact());
        block.nNonce   = (!fTestNet ? 92070 : 92081);

        if (true && (block.GetHash() != (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))) {
            // This will figure out a valid hash and Nonce if you're
            // creating a different genesis block:
            uint256 hashTarget = CBigNum().SetCompact(block.nBits).getuint256();
            while (block.GetHash() > hashTarget)
            {
                ++block.nNonce;
                if (block.nNonce == 0)
                {
                    LogPrintf("NONCE WRAPPED, incrementing time");
                    ++block.nTime;
                }
            }
        }
        //// debug print
        block.print();
        LogPrintf("block.GetHash() == %s\n", block.GetHash().ToString().c_str());
        LogPrintf("block.hashMerkleRoot == %s\n", block.hashMerkleRoot.ToString().c_str());
        LogPrintf("block.nTime = %u \n", block.nTime);
        LogPrintf("block.nNonce = %u \n", block.nNonce);

        if(!fTestNet)
            assert(block.hashMerkleRoot == uint256("0x80251aff18129581f06b3036bda4d571b909389699290deced973ebb580d11c5"));
        else
            assert(block.hashMerkleRoot == uint256("0x80251aff18129581f06b3036bda4d571b909389699290deced973ebb580d11c5"));

        block.print();
        assert(block.GetHash() == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet));
        assert(block.CheckBlock());

        // Start new block file
        unsigned int nFile;
        unsigned int nBlockPos;
        if (!block.WriteToDisk(nFile, nBlockPos))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!block.AddToBlockIndex(nFile, nBlockPos, hashGenesisBlock))
            return error("LoadBlockIndex() : genesis block not accepted");

        // ppcoin: initialize synchronized checkpoint
        if (!Checkpoints::WriteSyncCheckpoint((!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet)))
            return error("LoadBlockIndex() : failed to init sync checkpoint");
    }

    string strPubKey = "";

    // if checkpoint master key changed must reset sync-checkpoint
    if (!txdb.ReadCheckpointPubKey(strPubKey) || strPubKey != CSyncCheckpoint::strMasterPubKey)
    {
        // write checkpoint master key to db
        txdb.TxnBegin();
        if (!txdb.WriteCheckpointPubKey(CSyncCheckpoint::strMasterPubKey))
            return error("LoadBlockIndex() : failed to write new checkpoint master key to db");
        if (!txdb.TxnCommit())
            return error("LoadBlockIndex() : failed to commit new checkpoint master key to db");
        if ((!fTestNet) && !Checkpoints::ResetSyncCheckpoint())
            return error("LoadBlockIndex() : failed to reset sync-checkpoint");
    }

    return true;
}



void PrintBlockTree()
{
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (auto mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                LogPrintf("| ");
            LogPrintf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                LogPrintf("| ");
            LogPrintf("|\n");
        }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            LogPrintf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        LogPrintf("%d (%u,%u) %s  %08x  %s  mint %7s  tx %u",
               pindex->nHeight,
               pindex->nFile,
               pindex->nBlockPos,
               block.GetHash().ToString().c_str(),
               block.nBits,
               DateTimeStrFormat("%x %H:%M:%S", block.GetBlockTime()).c_str(),
               FormatMoney(pindex->nMint).c_str(),
               block.vtx.size());

        PrintWallets(block);

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

void PrintBlockInfo()
{
    LogPrintf("Blockchain information: [blocks = %d], [checkpoint-block-estimate = %d]\n",
              mapBlockIndex.size(), Checkpoints::GetTotalBlocksEstimate());
}

bool LoadExternalBlockFile(FILE* fileIn)
{
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    {
        LOCK(cs_main);

        try {
            CAutoFile blkdat(fileIn, SER_DISK, CLIENT_VERSION);
            unsigned int nPos = 0;

            while (nPos != (unsigned int)-1 && blkdat.good() && !fRequestShutdown)
            {
                unsigned char pchData[65536];

                do {
                    fseek(blkdat, nPos, SEEK_SET);
                    int nRead = fread(pchData, 1, sizeof(pchData), blkdat);

                    if (nRead <= 8)
                    {
                        nPos = (unsigned int)-1;
                        break;
                    }

                    void* nFind = memchr(pchData, pchMessageStart[0], nRead+1-sizeof(pchMessageStart));

                    if (nFind)
                    {
                        if (memcmp(nFind, pchMessageStart, sizeof(pchMessageStart))==0)
                        {
                            nPos += ((unsigned char*)nFind - pchData) + sizeof(pchMessageStart);
                            break;
                        }

                        nPos += ((unsigned char*)nFind - pchData) + 1;
                    }
                    else
                        nPos += sizeof(pchData) - sizeof(pchMessageStart) + 1;
                }
                while(!fRequestShutdown);

                if (nPos == (unsigned int)-1)
                    break;

                fseek(blkdat, nPos, SEEK_SET);
                unsigned int nSize;
                blkdat >> nSize;

                if (nSize > 0 && nSize <= MAX_BLOCK_SIZE)
                {
                    CBlock block;
                    blkdat >> block;

                    if (ProcessNewBlock(NULL,&block))
                    {
                        nLoaded++;
                        nPos += 4 + nSize;
                    }
                }
            }
        }
        catch (std::exception &e)
        {
            LogPrintf("%s : Deserialize or I/O error caught during load\n", __func__);
        }
    }

    LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // if detected invalid checkpoint enter safe mode
    if (Checkpoints::hashInvalidCheckpoint != 0)
    {
        nPriority = 3000;
        strStatusBar = strRPC = _("WARNING: Invalid checkpoint found! Displayed transactions may not be correct! You may need to upgrade, or notify developers.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
                if (nPriority > 1000)
                    strRPC = strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}

bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
    {
        bool txInMap = false;
        {
            LOCK(mempool.cs);
            txInMap = (mempool.exists(inv.hash));
        }

        return txInMap ||
                mapOrphanTransactions.count(inv.hash) ||
                txdb.ContainsTx(inv.hash);
    }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
                mapOrphanBlocks.count(inv.hash);

    case MSG_SPORK:
        return mapSporks.count(inv.hash);

    case MSG_MASTERNODE_WINNER:
        return mapSeenMasternodeVotes.count(inv.hash);

    }
    // Don't know what it is, just say we already got one
    return true;
}

void static ProcessGetData(CNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();
    vector<CInv> vNotFound;
    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end())
    {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv &inv = *it;
        if (fDebug) LogPrintf("ProcessGetData -- inv = %s\n", inv.ToString());
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                // Send block from disk
                auto mi = mapBlockIndex.find(inv.hash);

                if (mi != mapBlockIndex.end())
                {
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    pfrom->PushMessage(NetMsgType::BLOCK, block);

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // ppcoin: send latest proof-of-work block to allow the
                        // download node to accept as orphan (proof-of-stake
                        // block might be rejected by stake connection check)
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, GetLastBlockIndex(pindexBest, false)->GetBlockHash()));
                        pfrom->PushMessage(NetMsgType::INV, vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    if(mapDarksendBroadcastTxes.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss <<
                            mapDarksendBroadcastTxes[inv.hash].tx <<
                            mapDarksendBroadcastTxes[inv.hash].vin <<
                            mapDarksendBroadcastTxes[inv.hash].vchSig <<
                            mapDarksendBroadcastTxes[inv.hash].sigTime;

                        pfrom->PushMessage(NetMsgType::DSTX, ss);
                        pushed = true;
                    } else {
                        CTransaction tx;
                        if (mempool.lookup(inv.hash, tx)) {
                            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                            ss.reserve(1000);
                            ss << tx;
                            pfrom->PushMessage(NetMsgType::TX, ss);
                            pushed = true;
                        }
                    }
                }
                if (!pushed && inv.type == MSG_SPORK) {
                    if(mapSporks.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSporks[inv.hash];
                        pfrom->PushMessage(NetMsgType::SPORK, ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_MASTERNODE_WINNER) {
                    if(mapSeenMasternodeVotes.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        int a = 0;
                        ss.reserve(1000);
                        ss << mapSeenMasternodeVotes[inv.hash] << a;
                        pfrom->PushMessage(NetMsgType::MASTERNODEPAYMENTVOTE, ss);
                        pushed = true;
                    }
                }
            }

            // Track requests for our stuff
            Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage(NetMsgType::NOTFOUND, vNotFound);
    }
}

// The message start string is designed to be unlikely to occur in normal data.
// The characters are rarely used upper ASCII, not valid as UTF-8, and produce
// a large 4-byte int at any alignment.
unsigned char pchMessageStart[4] = { 0xb2, 0xd1, 0xf4, 0xa3 };

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    static int counter = 0;
    static map<CService, CPubKey> mapReuseKey;
    RandAddSeedPerfmon();

    if (fDebug)
    {
        LogPrintf("%s : received: %s (%u bytes) peer=%d (%s)\n", __func__, SanitizeString(strCommand),
                  vRecv.size(), pfrom->id, pfrom->addr.ToString().c_str());
    }

    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("%s : dropmessagestest [DROPPING RECV MESSAGE]\n", __func__);
        return true;
    }

    /* TODO: look into bloom */

    if (strCommand == NetMsgType::VERSION)
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            std::stringstream msg;
            msg << boost::format("%s : duplicate version message") % __func__;

            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_DUPLICATE, string("duplicate version message"));
            pfrom->Misbehaving(msg.str(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;

        if (pfrom->nVersion < ActiveProtocol())
        {
            std::stringstream msg;
            msg << boost::format("%s : peer=%d (%s) using obsolete version %i; disconnecting") % __func__ %
                pfrom->id % pfrom->addr.ToString().c_str() % pfrom->nVersion;

            // disconnect from peers older than this proto version
            LogPrintf("%s\n", msg.str().c_str());

            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("version must be %d or greater", ActiveProtocol()));

            pfrom->fDisconnect = true;
            pfrom->Misbehaving(msg.str(), 100);
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;

        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;

        if (!vRecv.empty())
        {
            vRecv >> pfrom->strSubVer;
            boost::replace_all(pfrom->strSubVer, "4.0.8.66", "4.0.2.666"); /* Special fix for the satanic edition */
        }

        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;

        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        pfrom->cleanSubVer = pfrom->strSubVer;

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        // record my external IP reported by peer
        if (addrFrom.IsRoutable() && addrMe.IsRoutable())
            addrSeenByPeer = addrMe;

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        if (GetBoolArg("-synctime", true))
            AddTimeData(pfrom->addr, nTime);

        // Change version
        pfrom->PushMessage(NetMsgType::VERACK);
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage(NetMsgType::GETADDR);
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        }
        else
        {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Ask the first connected node for block updates
        static int nAskedForBlocks = 0;

        if (!pfrom->fClient && !pfrom->fOneShot && (pfrom->nStartingHeight > (nBestHeight - 144)) &&
            (pfrom->nVersion < NOBLKS_VERSION_START || pfrom->nVersion >= NOBLKS_VERSION_END) &&
            (nAskedForBlocks < 1 || vNodes.size() <= 1))
        {
            if (fDebug)
                LogPrintf("%s : asking peer %d for block update from height %d\n",
                          __func__, pfrom->GetId(), pindexBest->nHeight);

            nAskedForBlocks++;
            pfrom->PushGetBlocks(pindexBest, uint256(0));
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);

            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        // Relay sync-checkpoint
        {
            LOCK(Checkpoints::cs_hashSyncCheckpoint);

            if (!Checkpoints::checkpointMessage.IsNull())
                Checkpoints::checkpointMessage.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        LogPrintf("%s : receive version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n",
                  __func__, pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString().c_str(),
                  addrFrom.ToString().c_str(), pfrom->addr.ToString().c_str());

        cPeerBlockCounts.input(pfrom->nStartingHeight);

        if (!IsInitialBlockDownload())
            Checkpoints::AskForPendingSyncCheckpoint(pfrom);

        // Be more aggressive with blockchain download. Send new getblocks() message after connection
        // to new node if waited longer than MAX_TIME_SINCE_BEST_BLOCK.
        int64_t timeSinceBestBlock = GetTime() - nTimeBestReceived;

        if (timeSinceBestBlock > MAX_TIME_SINCE_BEST_BLOCK)
        {
            LogPrintf("%s : Waiting %ld sec which is too long. Sending GetBlocks(0)\n", __func__, timeSinceBestBlock);
            pfrom->PushGetBlocks(pindexBest, uint256(0));
        }
    }
    else if (pfrom->nVersion == 0)
    {
        std::stringstream msg;
        msg << boost::format("%s : must initially send a version") % __func__;

        // Must have a version message before anything else
        // pfrom->Misbehaving(msg.str(), 1);
        // TODO: Add back the misbehavior when we have made sure the protocol comforms to this requirement
        return false;
    }
    else if (counter++ % PUSHGETBLOCKS_RESET_INTERVAL == 0 && !IsInitialBlockDownload())
    {
        pfrom->ResetPushGetBlocks();
        pfrom->PushGetBlocks(pindexBest, uint256(0));
        LogPrintf("%s : Force request of new blocks from peer %d\n", __func__, pfrom->id);
    }
    else if (strCommand == NetMsgType::VERACK)
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }
    else if (strCommand == NetMsgType::ADDR)
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;

        if (vAddr.size() > 1000)
        {
            std::stringstream msg;
            msg << boost::format("%s : message addr size() = %u") % __func__ % vAddr.size();

            pfrom->Misbehaving(msg.str(), 20);
            return error(msg.str().c_str());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            if (fShutdown)
                return true;

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;

            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);

            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);

                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;

                    if (hashSalt == 0)
                        hashSalt = GetRandHash();

                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr << 32) ^ ((GetTime()+hashAddr) / (24 * 60 * 60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;

                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;

                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }

                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)

                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin();
                         mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                    {
                        ((*mi).second)->PushAddress(addr);
                    }
                }
            }

            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }

        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);

        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;

        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }
    else if (strCommand == NetMsgType::INV)
    {
        vector<CInv> vInv;
        vRecv >> vInv;

        if (vInv.size() > MAX_INV_SZ)
        {
            std::stringstream msg;
            msg << boost::format("%s : message inv size() = %u") % __func__ % vInv.size();

            pfrom->Misbehaving(msg.str(), 20);
            return error(msg.str().c_str());
        }

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int) (-1);

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK)
            {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }

        CTxDB txdb("r");

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            if (fShutdown)
                return true;

            pfrom->AddInventoryKnown(inv);
            bool fAlreadyHave = AlreadyHave(txdb, inv);

            if (fDebug)
            {
                LogPrintf("%s : got inv: %s  %s peer=%d\n", __func__, inv.hash.ToString().c_str(),
                          fAlreadyHave ? "have" : "new", pfrom->GetId());
            }

            if (!fAlreadyHave)
                pfrom->AskFor(inv);
            else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash))
                pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(mapOrphanBlocks[inv.hash]));
            else if (nInv == nLastBlock)
            {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                pfrom->PushGetBlocks(mapBlockIndex[inv.hash], uint256(0));

                if (fDebug)
                    LogPrintf("%s : force request: %s\n", __func__, inv.ToString().c_str());
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }
    else if (strCommand == NetMsgType::GETDATA)
    {
        vector<CInv> vInv;
        vRecv >> vInv;

        if (vInv.size() > MAX_INV_SZ)
        {
            std::stringstream msg;
            msg << boost::format("%s : message getdata size() = %u") % __func__ % vInv.size();

            pfrom->Misbehaving(msg.str(), 20);
            return error(msg.str().c_str());
        }

        if (fDebugNet || (vInv.size() != 1))
            LogPrint("net", "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if ((fDebugNet && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint("net", "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }
    else if (strCommand == NetMsgType::GETBLOCKS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = locator.GetBlockIndex();

        // Send the rest of the chain
        if (pindex)
            pindex = pindex->pnext;
        int nLimit = 500;

        if (fDebug)
        {
            LogPrintf("%s : getblocks %d to %s limit %d from peer=%d\n", __func__, (pindex ? pindex->nHeight : -1),
                      hashStop == uint256(0) ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        }

        for (; pindex; pindex = pindex->pnext)
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                if (fDebug)
                {
                    LogPrintf("%s : getblocks stopping at %d %s\n", __func__, pindex->nHeight,
                              pindex->GetBlockHash().ToString().substr(0,20).c_str());
                }

                break;
            }

            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));

            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                if (fDebug)
                {
                    LogPrintf("%s : getblocks stopping at limit %d %s\n", __func__, pindex->nHeight,
                              pindex->GetBlockHash().ToString().substr(0,20).c_str());
                }

                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }
    else if (strCommand == "checkpoint")
    {
        CSyncCheckpoint checkpoint;
        vRecv >> checkpoint;

        if (fDebug)
            LogPrintf("checkpoint - Received: hash=%s", checkpoint.hashCheckpoint.ToString());

        if (checkpoint.ProcessSyncCheckpoint(pfrom))
        {
            // Relay
            pfrom->hashCheckpointKnown = checkpoint.hashCheckpoint;

            LOCK(cs_vNodes);

            BOOST_FOREACH(CNode* pnode, vNodes)
                checkpoint.RelayTo(pnode);
        }
    }
    else if (strCommand == NetMsgType::GETHEADERS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            auto mi = mapBlockIndex.find(hashStop);

            if (mi == mapBlockIndex.end())
                return true;

            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();

            if (pindex)
                pindex = pindex->pnext;
        }

        vector<CBlock> vHeaders;
        int nLimit = 2000;

        LogPrintf("%s : getheaders %d to %s\n", __func__, (pindex ? pindex->nHeight : -1),
                  hashStop.ToString().substr(0,20).c_str());

        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());

            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }

        pfrom->PushMessage(NetMsgType::HEADERS, vHeaders);
    }
    else if (strCommand == NetMsgType::TX || strCommand == NetMsgType::DSTX)
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CDataStream vMsg(vRecv);
        CTxDB txdb("r");
        CTransaction tx;

        //masternode signed transaction
        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;

        if(strCommand == NetMsgType::TX) {
            vRecv >> tx;
        } else if (strCommand == NetMsgType::DSTX) {
            //these allow masternodes to publish a limited amount of free transactions
            vRecv >> tx >> vin >> vchSig >> sigTime;

            BOOST_FOREACH(CMasternode& mn, vecMasternodes) {
                if(mn.vin == vin) {
                    if(!mn.allowFreeTx){
                        //multiple peers can send us a valid masternode transaction
                        if(fDebug) LogPrintf("dstx: Masternode sending too many transactions %s\n", tx.GetHash().ToString().c_str());
                        return true;
                    }

                    std::string strMessage = tx.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);

                    std::string errorMessage = "";
                    if(!darkSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)){
                        LogPrintf("dstx: Got bad masternode address signature %s \n", vin.ToString().c_str());
                        //pfrom->Misbehaving(20);
                        return false;
                    }

                    LogPrintf("dstx: Got Masternode transaction %s\n", tx.GetHash().ToString().c_str());
                    mn.allowFreeTx = false;

                    if(!mapDarksendBroadcastTxes.count(tx.GetHash())){
                        CDarksendBroadcastTx dstx;
                        dstx.tx = tx;
                        dstx.vin = vin;
                        dstx.vchSig = vchSig;
                        dstx.sigTime = sigTime;

                        mapDarksendBroadcastTxes.insert(make_pair(tx.GetHash(), dstx));
                    }
                }
            }
        }

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);
        bool fMissingInputs = false;

        if (tx.AcceptToMemoryPool(txdb, true, &fMissingInputs))
        {
            SyncWithWallets(tx, NULL, true);
            RelayTransaction(tx, inv.hash);
            mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end(); ++mi)
                {
                    const uint256& orphanTxHash = *mi;
                    CTransaction& orphanTx = mapOrphanTransactions[orphanTxHash];
                    bool fMissingInputs2 = false;

                    if (orphanTx.AcceptToMemoryPool(txdb, true, &fMissingInputs2))
                    {
                        LogPrintf("%s : accepted orphan tx %s\n", __func__,
                                  orphanTxHash.ToString().substr(0,10).c_str());

                        SyncWithWallets(tx, NULL, true);
                        RelayTransaction(orphanTx, orphanTxHash);
                        mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanTxHash));
                        vWorkQueue.push_back(orphanTxHash);
                        vEraseQueue.push_back(orphanTxHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid orphan
                        vEraseQueue.push_back(orphanTxHash);

                        LogPrintf("%s : removed invalid orphan tx %s\n", __func__,
                                  orphanTxHash.ToString().substr(0,10).c_str());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);

            if (nEvicted > 0)
                LogPrintf("mapOrphan overflow, removed %u tx\n", nEvicted);
        }

        if (tx.nDoS)
            pfrom->Misbehaving(std::string("transaction misbehavior"), tx.nDoS);
    }
    else if (strCommand == NetMsgType::BLOCK)
    {
        CBlock block;
        vRecv >> block;

        if (fDebug)
            LogPrintf("%s : received block %s\n", __func__, block.GetHash().ToString().c_str());

        CInv inv(MSG_BLOCK, block.GetHash());
        pfrom->AddInventoryKnown(inv);

        if (ProcessNewBlock(pfrom, &block))
            mapAlreadyAskedFor.erase(inv);
        else
        {
            // Be more aggressive with blockchain download. Send getblocks() message after
            // an error related to new block download
            int64_t timeSinceBestBlock = GetTime() - nTimeBestReceived;

            if (timeSinceBestBlock > MAX_TIME_SINCE_BEST_BLOCK)
            {
                LogPrintf("%s : Waiting %ld sec which is too long. Sending GetBlocks(0)\n", __func__, timeSinceBestBlock);
                pfrom->PushGetBlocks(pindexBest, uint256(0));
            }
        }

        if (block.nDoS)
            pfrom->Misbehaving(std::string("block misbehavior"), block.nDoS);
    }
    else if (strCommand == NetMsgType::GETADDR)
    {
        // Don't return addresses older than nCutOff timestamp
        int64_t nCutOff = GetTime() - (nNodeLifespan * 24 * 60 * 60);
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            if(addr.nTime > nCutOff)
                pfrom->PushAddress(addr);
    }
    else if (strCommand == NetMsgType::MEMPOOL)
    {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;

        for (unsigned int i = 0; i < vtxid.size(); i++) {
            CInv inv(MSG_TX, vtxid[i]);
            vInv.push_back(inv);
            if (i == (MAX_INV_SZ - 1))
                break;
        }

        if (vInv.size() > 0)
            pfrom->PushMessage(NetMsgType::INV, vInv);
    }
    else if (strCommand == NetMsgType::PING)
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage(NetMsgType::PONG, nonce);
        }
    }
    else if (strCommand == NetMsgType::ALERT)
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);

                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(std::string("alert misbehavior"), 10);
            }
        }
    }
    else if (strCommand == NetMsgType::REJECT)
    {
        if (fDebug) {
            try {
                string strMsg; unsigned char ccode; string strReason;
                vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

                ostringstream ss;
                ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

                if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX)
                {
                    uint256 hash;
                    vRecv >> hash;
                    ss << ": hash " << hash.ToString();
                }
                LogPrint("net", "Reject %s\n", SanitizeString(ss.str()));
            } catch (const std::ios_base::failure&) {
                // Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint("net", "Unparseable reject message received\n");
            }
        }
    }
    else
    {
        bool found = false;
        const std::vector<std::string> &allMessages = getAllNetMessageTypes();
        BOOST_FOREACH(const std::string msg, allMessages) {
            if(msg == strCommand) {
                found = true;
                break;
            }
        }

        if (found)
        {
            //probably one the extensions

            // TODO: Test/Enable Darksend
            /* DSF, DSC, DSA, DSQ, DSI, DSSUB, DSSU, DSS */
            //ProcessMessageDarksend(pfrom, strCommand, vRecv);

            /* DSEE, DSEEP, DSEG, MNGET, MNW */
            ProcessMessageMasternode(pfrom, strCommand, vRecv);

            // TODO: Test/Enable InstantX
            /* TXLREQ, TXLVOTE */
            //ProcessMessageInstantX(pfrom, strCommand, vRecv);

            /* SPORK, GETSPORKS */
            sporkManager.ProcessSpork(pfrom, strCommand, vRecv);
        }
        else
        {
            // Ignore unknown commands for extensibility
            LogPrint("net", "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
        }
    }


    // TODO: refactor this, either not needed or probably should not be done here
    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == NetMsgType::VERSION || strCommand == NetMsgType::ADDR || strCommand == NetMsgType::INV || strCommand == NetMsgType::GETDATA || strCommand == NetMsgType::PING)
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

int ActiveProtocol()
{
    if (sporkManager.IsSporkActive(SPORK_13_PROTOCOL_V4_ENFORCEMENT))
    {
            return MIN_PEER_PROTO_VERSION_AFTER_V4_ENFORCEMENT;
       } else {
            return MIN_PEER_PROTO_VERSION_AFTER_V301_ENFORCEMENT_AND_MNENFORCE;
  }
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
    //if (fDebug)
    //    LogPrintf("%s(%u messages)\n", __func__, pfrom->vRecvMsg.size());

    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data

    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end())
    {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        //if (fDebug)
        //    LogPrintf("%s(message %u msgsz, %u bytes, complete:%s)\n", __func__,
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, pchMessageStart, sizeof(pchMessageStart)) != 0)
        {
            LogPrintf("%s: INVALID MESSAGESTART %s peer=%d\n", __func__,
                      SanitizeString(msg.hdr.GetCommand()), pfrom->id);
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;

        if (!hdr.IsValid())
        {
            LogPrintf("%s: ERRORS IN HEADER %s peer=%d\n", __func__,
                      SanitizeString(hdr.GetCommand()), pfrom->id);
            continue;
        }

        string strCommand = hdr.GetCommand();
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));

        if (nChecksum != hdr.nChecksum)
        {
            LogPrintf("%s(%s, %u bytes): CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n", __func__,
                      SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;

        try
        {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        }
        catch (const std::ios_base::failure& e)
        {
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_MALFORMED, string("error parsing message"));

            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught, normally caused by a message"
                          "being shorter than its stated length\n", __func__, SanitizeString(strCommand),
                          nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught\n", __func__,
                          SanitizeString(strCommand), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (const boost::thread_interrupted&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            PrintExceptionContinue(&e, "ProcessMessages()");
        }
        catch (...)
        {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
        {
            LogPrintf("%s(%s, %u bytes) FAILED peer=%d\n", __func__, SanitizeString(strCommand),
                      nMessageSize, pfrom->id);
        }

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
    ENTER_CRITICAL_SECTION(cs_vNodes);
    TRY_LOCK(cs_main, lockMain);
    TRY_LOCK(pto->cs_vSend, lockSend);

    if (!lockMain || !lockSend)
    {
        LEAVE_CRITICAL_SECTION(cs_vNodes);
        return true;
    }

    // Dont send anything until we get their version message
    if (pto->nVersion == 0)
    {
        LEAVE_CRITICAL_SECTION(cs_vNodes);
        return true;
    }

    // Keep-alive ping. We send a nonce of zero because we don't use it anywhere right now
    if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSendMsg.empty())
    {
        uint64_t nonce = 0;

        if (pto->nVersion > BIP0031_VERSION)
            pto->PushMessage(NetMsgType::PING, nonce);
        else
            pto->PushMessage(NetMsgType::PING);
    }

    // Resend wallet transactions that haven't gotten in a block yet
    ResendWalletTransactions();

    // Address refresh broadcast
    static int64_t nLastRebroadcast;

    if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
    {
        {
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                // Periodically clear setAddrKnown to allow refresh broadcasts
                if (nLastRebroadcast)
                    pnode->setAddrKnown.clear();

                // Rebroadcast our address
                if (fListen)
                {
                    CAddress addr = GetLocalAddress(&pnode->addr);

                    if (addr.IsRoutable())
                        pnode->PushAddress(addr);
                }
            }
        }

        nLastRebroadcast = GetTime();
    }

    LEAVE_CRITICAL_SECTION(cs_vNodes);

    // Message: addr
    if (fSendTrickle)
    {
        vector<CAddress> vAddr;
        vAddr.reserve(pto->vAddrToSend.size());

        BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
        {
            // Returns true if wasn't already contained in the set
            if (pto->setAddrKnown.insert(addr).second)
            {
                vAddr.push_back(addr);

                // Receiver rejects addr messages larger than 1000
                if (vAddr.size() >= 1000)
                {
                    pto->PushMessage(NetMsgType::ADDR, vAddr);
                    vAddr.clear();
                }
            }
        }

        pto->vAddrToSend.clear();

        if (!vAddr.empty())
            pto->PushMessage(NetMsgType::ADDR, vAddr);
    }

    // Message: inventory
    vector<CInv> vInv;
    vector<CInv> vInvWait;

    {
        LOCK(pto->cs_inventory);

        vInv.reserve(pto->vInventoryToSend.size());
        vInvWait.reserve(pto->vInventoryToSend.size());

        BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
        {
            if (pto->setInventoryKnown.count(inv))
                continue;

            // Trickle out tx inv to protect privacy
            if (inv.type == MSG_TX && !fSendTrickle)
            {
                // 1 / 4 of tx invs blast to all immediately
                static uint256 hashSalt;

                if (hashSalt == 0)
                    hashSalt = GetRandHash();

                uint256 hashRand = inv.hash ^ hashSalt;
                hashRand = Hash(BEGIN(hashRand), END(hashRand));
                bool fTrickleWait = ((hashRand & 3) != 0);

                // Always trickle our own transactions
                if (!fTrickleWait)
                {
                    CWalletTx wtx;

                    if (GetTransaction(inv.hash, wtx))
                    {
                        if (wtx.fFromMe)
                            fTrickleWait = true;
                    }
                }

                if (fTrickleWait)
                {
                    vInvWait.push_back(inv);
                    continue;
                }
            }

            // Returns true if wasn't already contained in the set
            if (pto->setInventoryKnown.insert(inv).second)
            {
                vInv.push_back(inv);

                if (vInv.size() >= 1000)
                {
                    pto->PushMessage(NetMsgType::INV, vInv);
                    vInv.clear();
                }
            }
        }

        pto->vInventoryToSend = vInvWait;
    }

    if (!vInv.empty())
        pto->PushMessage(NetMsgType::INV, vInv);

    // Message: getdata
    vector<CInv> vGetData;
    int64_t nNow = GetTime() * 1000000;
    CTxDB txdb("r");

    while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
    {
        const CInv& inv = (*pto->mapAskFor.begin()).second;

        if (!AlreadyHave(txdb, inv))
        {
            if (fDebugNet)
                LogPrintf("sending getdata: %s\n", inv.ToString().c_str());

            vGetData.push_back(inv);

            if (vGetData.size() >= 1000)
            {
                pto->PushMessage(NetMsgType::GETDATA, vGetData);
                vGetData.clear();
            }

            mapAlreadyAskedFor[inv] = nNow;
        }

        pto->mapAskFor.erase(pto->mapAskFor.begin());
    }

    if (!vGetData.empty())
        pto->PushMessage(NetMsgType::GETDATA, vGetData);

    return true;
}

CScript GetDeveloperScript()
{
    string strAddress;

    // if (sporkManager.IsSporkActive(SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT)) {
    //     // v2.0.1
    //     strAddress = fTestNet ? DEVELOPER_ADDRESS_TESTNET_V2 : DEVELOPER_ADDRESS_MAINNET_V2;
    // } else {
    //     // v2.0.0
    //     strAddress = fTestNet ? DEVELOPER_ADDRESS_TESTNET_V1 : DEVELOPER_ADDRESS_MAINNET_V1;
    // }

    // if (sporkManager.IsSporkActive(SPORK_10_V3_DEV_PAYMENTS_ENFORCEMENT)) {
    //     // v3.0.0
    //     strAddress = fTestNet ? DEVELOPER_ADDRESS_TESTNET_V3 : DEVELOPER_ADDRESS_MAINNET_V3;
    // } else {
    //     // v2.0.1
    //     strAddress = fTestNet ? DEVELOPER_ADDRESS_TESTNET_V2 : DEVELOPER_ADDRESS_MAINNET_V2;
    // }

    // v3.0.0+ default
    strAddress = fTestNet ? DEVELOPER_ADDRESS_TESTNET_V3 : DEVELOPER_ADDRESS_MAINNET_V3;

    return GetScriptForDestination(CBitcoinAddress(strAddress).Get());
}

int64_t GetDeveloperPayment(int64_t nBlockValue)
{
    // if (sporkManager.IsSporkActive(SPORK_6_UPDATED_DEV_PAYMENTS_ENFORCEMENT)) {
    //     // v2.0.1
    //     return nBlockValue * DEVELOPER_PAYMENT_V2 / COIN;
    // }

    // v2.0.0
    // return nBlockValue * DEVELOPER_PAYMENT_V1 / COIN;

    // v3.0.0+ default
    return nBlockValue * DEVELOPER_PAYMENT_V2 / COIN;
}

int64_t GetMasternodePayment(int nHeight, int64_t blockValue)
{
    int64_t nDeveloperPayment = GetDeveloperPayment(blockValue);
    return (blockValue - nDeveloperPayment) * 66 / 100; // 66%
}