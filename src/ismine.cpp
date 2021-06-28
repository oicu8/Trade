// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ismine.h"

#include "key.h"
#include "keystore.h"
#include "script.h"
#include "script/standard.h"

#include <boost/foreach.hpp>

typedef std::vector<unsigned char> valtype;

unsigned int HaveKeys(const std::vector<valtype>& pubkeys, const CKeyStore& keystore)
{
    unsigned int nResult = 0;
    BOOST_FOREACH(const valtype& pubkey, pubkeys)
    {
        CKeyID keyID = CPubKey(pubkey).GetID();
        if (keystore.HaveKey(keyID))
            ++nResult;
    }
    return nResult;
}

class CKeyStoreIsMineVisitor : public boost::static_visitor<bool>
{
private:
    const CKeyStore *keystore;
public:
    CKeyStoreIsMineVisitor(const CKeyStore *keystoreIn) : keystore(keystoreIn) { }
    bool operator()(const CNoDestination &dest) const { return false; }
    bool operator()(const CKeyID &keyID) const { return keystore->HaveKey(keyID); }
    bool operator()(const CScriptID &scriptID) const { return keystore->HaveCScript(scriptID); }
};

bool IsMine(const CKeyStore &keystore, const CTxDestination &dest)
{
    return boost::apply_visitor(CKeyStoreIsMineVisitor(&keystore), dest);
}

bool IsMine(const CKeyStore &keystore, const CScript& scriptPubKey)
{
    std::vector<valtype> vSolutions;
    txnouttype whichType;
    if (!Solver(scriptPubKey, whichType, vSolutions))
        return false;

    CKeyID keyID;
    switch (whichType)
    {
    case TX_NONSTANDARD:
    case TX_NULL_DATA:
        return false;
    case TX_PUBKEY:
        keyID = CPubKey(vSolutions[0]).GetID();
        return keystore.HaveKey(keyID);
    case TX_PUBKEYHASH:
        keyID = CKeyID(uint160(vSolutions[0]));
        return keystore.HaveKey(keyID);
    case TX_SCRIPTHASH:
    {
        CScript subscript;
        if (!keystore.GetCScript(CScriptID(uint160(vSolutions[0])), subscript))
            return false;
        return IsMine(keystore, subscript);
    }
    case TX_MULTISIG:
    {
        // Only consider transactions "mine" if we own ALL the
        // keys involved. multi-signature transactions that are
        // partially owned (somebody else has a key that can spend
        // them) enable spend-out-from-under-you attacks, especially
        // in shared-wallet situations.
        std::vector<valtype> keys(vSolutions.begin()+1, vSolutions.begin()+vSolutions.size()-1);
        return HaveKeys(keys, keystore) == keys.size();
    }
    }
    return false;
}
