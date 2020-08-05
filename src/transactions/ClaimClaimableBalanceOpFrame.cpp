// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ClaimClaimableBalanceOpFrame.h"
#include "crypto/SHA.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

ClaimClaimableBalanceOpFrame::ClaimClaimableBalanceOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mClaimClaimableBalance(mOperation.body.claimClaimableBalanceOp())
{
}

ThresholdLevel
ClaimClaimableBalanceOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
ClaimClaimableBalanceOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 14;
}

bool
validatePredicate(ClaimPredicate const& pred, TimePoint closeTime)
{
    switch (pred.type())
    {
    case CLAIM_PREDICATE_UNCONDITIONAL:
        break;
    case CLAIM_PREDICATE_AND:
    {
        auto const& andPredicates = pred.andPredicates();

        return validatePredicate(andPredicates.at(0), closeTime) &&
               validatePredicate(andPredicates.at(1), closeTime);
    }

    case CLAIM_PREDICATE_OR:
    {
        auto const& orPredicates = pred.orPredicates();

        return validatePredicate(orPredicates.at(0), closeTime) ||
               validatePredicate(orPredicates.at(1), closeTime);
    }
    case CLAIM_PREDICATE_NOT:
        return !validatePredicate(*pred.notPredicate(), closeTime);
    case CLAIM_PREDICATE_BEFORE_ABSOLUTE_TIME:
        return static_cast<uint64_t>(pred.absBefore()) > closeTime;
    default:
        throw std::runtime_error("Invalid ClaimPredicate");
    }

    return true;
}

bool
ClaimClaimableBalanceOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    auto claimableBalanceLtxEntry =
        stellar::loadClaimableBalance(ltx, mClaimClaimableBalance.balanceID);
    if (!claimableBalanceLtxEntry)
    {
        innerResult().code(CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST);
        return false;
    }

    auto const& claimableBalance =
        claimableBalanceLtxEntry.current().data.claimableBalance();

    auto it = std::find_if(
        std::begin(claimableBalance.claimants),
        std::end(claimableBalance.claimants), [&](Claimant const& claimant) {
            return claimant.v0().destination == getSourceID();
        });

    auto header = ltx.loadHeader();
    if (it == claimableBalance.claimants.end() ||
        !validatePredicate(it->v0().predicate,
                           header.current().scpValue.closeTime))
    {
        innerResult().code(CLAIM_CLAIMABLE_BALANCE_CANNOT_CLAIM);
        return false;
    }

    // Try to give reserve back to createdBy account. If we can't, try to give
    // it back to sourceAccount
    if (!loadAccountAndAddBalance(ltx, header, claimableBalance.createdBy,
                                  claimableBalance.reserve))
    {
        if (!loadAccountAndAddBalance(ltx, header, getSourceID(),
                                      claimableBalance.reserve))
        {
            innerResult().code(CLAIM_CLAIMABLE_BALANCE_LINE_FULL);
            return false;
        }
    }

    auto const& asset = claimableBalance.asset;
    auto amount = claimableBalance.amount;
    if (asset.type() == ASSET_TYPE_NATIVE)
    {
        if (!loadAccountAndAddBalance(ltx, header, getSourceID(), amount))
        {
            innerResult().code(CLAIM_CLAIMABLE_BALANCE_LINE_FULL);
            return false;
        }
    }
    else
    {
        auto trustline = loadTrustLine(ltx, getSourceID(), asset);
        if (!trustline)
        {
            innerResult().code(CLAIM_CLAIMABLE_BALANCE_NO_TRUST);
            return false;
        }
        if (!trustline.isAuthorized())
        {
            innerResult().code(CLAIM_CLAIMABLE_BALANCE_NOT_AUTHORIZED);
            return false;
        }
        if (!trustline.addBalance(header, amount))
        {
            innerResult().code(CLAIM_CLAIMABLE_BALANCE_LINE_FULL);
            return false;
        }
    }

    claimableBalanceLtxEntry.erase();

    innerResult().code(CLAIM_CLAIMABLE_BALANCE_SUCCESS);
    return true;
}

bool
ClaimClaimableBalanceOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
}

bool
ClaimClaimableBalanceOpFrame::loadAccountAndAddBalance(
    AbstractLedgerTxn& ltx, LedgerTxnHeader const& header,
    AccountID const& accountID, int64_t amount)
{
    auto account = stellar::loadAccount(ltx, accountID);
    return account && addBalance(header, account, amount);
}

void
ClaimClaimableBalanceOpFrame::insertLedgerKeysToPrefetch(
    std::unordered_set<LedgerKey>& keys) const
{
    keys.emplace(claimableBalanceKey(mClaimClaimableBalance.balanceID));
}
}
