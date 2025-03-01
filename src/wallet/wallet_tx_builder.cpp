// Copyright (c) 2022 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "wallet/wallet_tx_builder.h"

using namespace libzcash;

int GetAnchorHeight(const CChain& chain, uint32_t anchorConfirmations)
{
    int nextBlockHeight = chain.Height() + 1;
    return nextBlockHeight - anchorConfirmations;
}

PrepareTransactionResult WalletTxBuilder::PrepareTransaction(
        CWallet& wallet,
        const ZTXOSelector& selector,
        SpendableInputs& spendable,
        const std::vector<Payment>& payments,
        const CChain& chain,
        TransactionStrategy strategy,
        CAmount fee,
        uint32_t anchorConfirmations) const
{
    assert(fee < MAX_MONEY);

    int anchorHeight = GetAnchorHeight(chain, anchorConfirmations);
    auto selected = ResolveInputsAndPayments(wallet, selector, spendable, payments, chain, strategy, fee, anchorHeight);
    if (std::holds_alternative<InputSelectionError>(selected)) {
        return std::get<InputSelectionError>(selected);
    }

    auto resolvedSelection = std::get<InputSelection>(selected);
    auto resolvedPayments = resolvedSelection.GetPayments();

    // We do not set a change address if there is no change.
    std::optional<ChangeAddress> changeAddr;
    auto changeAmount = spendable.Total() - resolvedPayments.Total() - fee;
    if (changeAmount > 0) {
        // Determine the account we're sending from.
        auto sendFromAccount = wallet.FindAccountForSelector(selector).value_or(ZCASH_LEGACY_ACCOUNT);

        auto getAllowedChangePools = [&](const std::set<ReceiverType>& receiverTypes) {
            std::set<OutputPool> result{resolvedPayments.GetRecipientPools()};
            // We always allow shielded change when not sending from the legacy account.
            if (sendFromAccount != ZCASH_LEGACY_ACCOUNT) {
                result.insert(OutputPool::Sapling);
            }
            for (ReceiverType rtype : receiverTypes) {
                switch (rtype) {
                    case ReceiverType::P2PKH:
                    case ReceiverType::P2SH:
                        // TODO: This is the correct policy, but it’s a breaking change from
                        //       previous behavior, so enable it separately. (#6409)
                        // if (strategy.AllowRevealedRecipients()) {
                        if (!spendable.utxos.empty() || strategy.AllowRevealedRecipients()) {
                            result.insert(OutputPool::Transparent);
                        }
                        break;
                    case ReceiverType::Sapling:
                        if (!spendable.saplingNoteEntries.empty() || strategy.AllowRevealedAmounts()) {
                            result.insert(OutputPool::Sapling);
                        }
                        break;
                    case ReceiverType::Orchard:
                        if (params.GetConsensus().NetworkUpgradeActive(anchorHeight, Consensus::UPGRADE_NU5)
                            && (!spendable.orchardNoteMetadata.empty() || strategy.AllowRevealedAmounts())) {
                            result.insert(OutputPool::Orchard);
                        }
                        break;
                }
            }
            return result;
        };

        auto addChangePayment = [&](const std::optional<RecipientAddress>& sendTo) {
            assert(sendTo.has_value());
            resolvedPayments.AddPayment(
                    ResolvedPayment(std::nullopt, sendTo.value(), changeAmount, std::nullopt, true));
            return sendTo.value();
        };

        auto changeAddressForTransparentSelector = [&](const std::set<ReceiverType>& receiverTypes) {
            return addChangePayment(
                    wallet.GenerateChangeAddressForAccount(
                            sendFromAccount,
                            getAllowedChangePools(receiverTypes)));
        };

        auto changeAddressForSaplingAddress = [&](const libzcash::SaplingPaymentAddress& addr) {
            // for Sapling, if using a legacy address, return change to the
            // originating address; otherwise return it to the Sapling internal
            // address corresponding to the UFVK.
            return addChangePayment(
                    sendFromAccount == ZCASH_LEGACY_ACCOUNT
                    ? addr
                    : wallet.GenerateChangeAddressForAccount(
                            sendFromAccount,
                            getAllowedChangePools({ReceiverType::Sapling})));
        };

        auto changeAddressForZUFVK = [&](
                const ZcashdUnifiedFullViewingKey& zufvk,
                const std::set<ReceiverType>& receiverTypes) {
            return addChangePayment(zufvk.GetChangeAddress(getAllowedChangePools(receiverTypes)));
        };

        changeAddr = examine(selector.GetPattern(), match {
            [&](const CKeyID&) -> ChangeAddress {
                return changeAddressForTransparentSelector({ReceiverType::P2PKH});
            },
            [&](const CScriptID&) -> ChangeAddress {
                return changeAddressForTransparentSelector({ReceiverType::P2SH});
            },
            [](const libzcash::SproutPaymentAddress& addr) -> ChangeAddress {
                // for Sprout, we return change to the originating address using the tx builder.
                return addr;
            },
            [](const libzcash::SproutViewingKey& vk) -> ChangeAddress {
                // for Sprout, we return change to the originating address using the tx builder.
                return vk.address();
            },
            [&](const libzcash::SaplingPaymentAddress& addr) -> ChangeAddress {
                return changeAddressForSaplingAddress(addr);
            },
            [&](const libzcash::SaplingExtendedFullViewingKey& fvk) -> ChangeAddress {
                return changeAddressForSaplingAddress(fvk.DefaultAddress());
            },
            [&](const libzcash::UnifiedAddress& ua) -> ChangeAddress {
                auto zufvk = wallet.GetUFVKForAddress(ua);
                assert(zufvk.has_value());
                return changeAddressForZUFVK(zufvk.value(), ua.GetKnownReceiverTypes());
            },
            [&](const libzcash::UnifiedFullViewingKey& fvk) -> ChangeAddress {
                return changeAddressForZUFVK(
                        ZcashdUnifiedFullViewingKey::FromUnifiedFullViewingKey(params, fvk),
                        fvk.GetKnownReceiverTypes());
            },
            [&](const AccountZTXOPattern& acct) -> ChangeAddress {
                return addChangePayment(
                        wallet.GenerateChangeAddressForAccount(
                                acct.GetAccountId(),
                                getAllowedChangePools(acct.GetReceiverTypes())));
            }
        });
    }

    auto ovks = SelectOVKs(wallet, selector, spendable);

    auto effects = TransactionEffects(
            anchorConfirmations,
            spendable,
            resolvedPayments,
            changeAddr,
            fee,
            ovks.first,
            ovks.second,
            anchorHeight);
    effects.LockSpendable(wallet);
    return effects;
}

Payments InputSelection::GetPayments() const {
    return this->payments;
}

CAmount WalletTxBuilder::DefaultDustThreshold() const {
    CKey secret{CKey::TestOnlyRandomKey(true)};
    CScript scriptPubKey = GetScriptForDestination(secret.GetPubKey().GetID());
    CTxOut txout(CAmount(1), scriptPubKey);
    return txout.GetDustThreshold(minRelayFee);
}

SpendableInputs WalletTxBuilder::FindAllSpendableInputs(
        const CWallet& wallet,
        const ZTXOSelector& selector,
        int32_t minDepth) const
{
    LOCK2(cs_main, wallet.cs_wallet);
    return wallet.FindSpendableInputs(selector, minDepth, std::nullopt);
}

InputSelectionResult WalletTxBuilder::ResolveInputsAndPayments(
        const CWallet& wallet,
        const ZTXOSelector& selector,
        SpendableInputs& spendableMut,
        const std::vector<Payment>& payments,
        const CChain& chain,
        TransactionStrategy strategy,
        CAmount fee,
        int anchorHeight) const
{
    LOCK2(cs_main, wallet.cs_wallet);

    // Determine the target totals
    CAmount sendAmount{0};
    for (const auto& payment : payments) {
        sendAmount += payment.GetAmount();
    }
    CAmount targetAmount = sendAmount + fee;

    // This is a simple greedy algorithm to attempt to preserve requested
    // transactional privacy while moving as much value to the most recent pool
    // as possible.  This will also perform opportunistic shielding if the
    // transaction strategy permits.

    CAmount maxSaplingAvailable = spendableMut.GetSaplingTotal();
    CAmount maxOrchardAvailable = spendableMut.GetOrchardTotal();
    uint32_t orchardOutputs{0};

    // we can only select Orchard addresses if there are sufficient non-Sprout
    // funds to cover the total payments + fee.
    bool canResolveOrchard =
        params.GetConsensus().NetworkUpgradeActive(anchorHeight, Consensus::UPGRADE_NU5)
        && spendableMut.Total() - spendableMut.GetSproutTotal() >= targetAmount;
    std::vector<ResolvedPayment> resolvedPayments;
    std::optional<AddressResolutionError> resolutionError;
    for (const auto& payment : payments) {
        examine(payment.GetAddress(), match {
            [&](const CKeyID& p2pkh) {
                if (strategy.AllowRevealedRecipients()) {
                    resolvedPayments.emplace_back(
                            std::nullopt, p2pkh, payment.GetAmount(), payment.GetMemo(), false);
                } else {
                    resolutionError = AddressResolutionError::TransparentRecipientNotAllowed;
                }
            },
            [&](const CScriptID& p2sh) {
                if (strategy.AllowRevealedRecipients()) {
                    resolvedPayments.emplace_back(
                            std::nullopt, p2sh, payment.GetAmount(), payment.GetMemo(), false);
                } else {
                    resolutionError = AddressResolutionError::TransparentRecipientNotAllowed;
                }
            },
            [&](const SproutPaymentAddress&) {
                resolutionError = AddressResolutionError::SproutRecipientsNotSupported;
            },
            [&](const SaplingPaymentAddress& addr) {
                if (strategy.AllowRevealedAmounts() || payment.GetAmount() <= maxSaplingAvailable) {
                    resolvedPayments.emplace_back(
                            std::nullopt, addr, payment.GetAmount(), payment.GetMemo(), false);
                    if (!strategy.AllowRevealedAmounts()) {
                        maxSaplingAvailable -= payment.GetAmount();
                    }
                } else {
                    resolutionError = AddressResolutionError::RevealingSaplingAmountNotAllowed;
                }
            },
            [&](const UnifiedAddress& ua) {
                if (canResolveOrchard
                    && ua.GetOrchardReceiver().has_value()
                    && (strategy.AllowRevealedAmounts() || payment.GetAmount() <= maxOrchardAvailable)
                    ) {
                    resolvedPayments.emplace_back(
                        ua, ua.GetOrchardReceiver().value(), payment.GetAmount(), payment.GetMemo(), false);
                    if (!strategy.AllowRevealedAmounts()) {
                        maxOrchardAvailable -= payment.GetAmount();
                    }
                    orchardOutputs += 1;
                } else if (ua.GetSaplingReceiver().has_value()
                    && (strategy.AllowRevealedAmounts() || payment.GetAmount() <= maxSaplingAvailable)
                    ) {
                    resolvedPayments.emplace_back(
                        ua, ua.GetSaplingReceiver().value(), payment.GetAmount(), payment.GetMemo(), false);
                    if (!strategy.AllowRevealedAmounts()) {
                        maxSaplingAvailable -= payment.GetAmount();
                    }
                } else {
                    if (strategy.AllowRevealedRecipients()) {
                        if (ua.GetP2SHReceiver().has_value()) {
                            resolvedPayments.emplace_back(
                                ua, ua.GetP2SHReceiver().value(), payment.GetAmount(), std::nullopt, false);
                        } else if (ua.GetP2PKHReceiver().has_value()) {
                            resolvedPayments.emplace_back(
                                ua, ua.GetP2PKHReceiver().value(), payment.GetAmount(), std::nullopt, false);
                        } else {
                            // This should only occur when we have
                            // • an Orchard-only UA,
                            // • `AllowRevealedRecipients`, and
                            // • can’t resolve Orchard (which means either insufficient non-Sprout
                            //   funds or pre-NU5).
                            resolutionError = AddressResolutionError::CouldNotResolveReceiver;
                        }
                    } else if (strategy.AllowRevealedAmounts()) {
                        resolutionError = AddressResolutionError::TransparentReceiverNotAllowed;
                    } else {
                        resolutionError = AddressResolutionError::RevealingReceiverAmountsNotAllowed;
                    }
                }
            }
        });

        if (resolutionError.has_value()) {
            return resolutionError.value();
        }
    }
    auto resolved = Payments(resolvedPayments);

    if (orchardOutputs > this->maxOrchardActions) {
        return ExcessOrchardActionsError(
                ActionSide::Output,
                orchardOutputs,
                this->maxOrchardActions);
    }

    // Set the dust threshold so that we can select enough inputs to avoid
    // creating dust change amounts.
    CAmount dustThreshold{this->DefaultDustThreshold()};

    // TODO: the set of recipient pools is not quite sufficient information here; we should
    // probably perform note selection at the same time as we're performing resolved payment
    // construction above.
    if (!spendableMut.LimitToAmount(targetAmount, dustThreshold, resolved.GetRecipientPools())) {
        CAmount changeAmount{spendableMut.Total() - targetAmount};
        return InvalidFundsError(
                spendableMut.Total(),
                changeAmount > 0 && changeAmount < dustThreshold
                // TODO: we should provide the option for the caller to explicitly
                // forego change (definitionally an amount below the dust amount)
                // and send the extra to the recipient or the miner fee to avoid
                // creating dust change, rather than prohibit them from sending
                // entirely in this circumstance.
                // (Daira disagrees, as this could leak information to the recipient
                // or publicly in the fee.)
                ? InvalidFundsReason(DustThresholdError(dustThreshold, changeAmount))
                : InvalidFundsReason(InsufficientFundsError(targetAmount)));
    }

    // When spending transparent coinbase outputs, all inputs must be fully
    // consumed, and they may only be sent to shielded recipients.
    if (spendableMut.HasTransparentCoinbase()) {
        if (spendableMut.Total() != targetAmount) {
            return ChangeNotAllowedError(spendableMut.Total(), targetAmount);
        } else if (resolved.HasTransparentRecipient()) {
            return AddressResolutionError::TransparentRecipientNotAllowed;
        }
    }

    if (spendableMut.orchardNoteMetadata.size() > this->maxOrchardActions) {
        return ExcessOrchardActionsError(
                ActionSide::Input,
                spendableMut.orchardNoteMetadata.size(),
                this->maxOrchardActions);
    }

    return InputSelection(resolved, anchorHeight);
}

std::pair<uint256, uint256>
GetOVKsForUFVK(const UnifiedFullViewingKey& ufvk, const SpendableInputs& spendable)
{
    if (!spendable.orchardNoteMetadata.empty()) {
        auto fvk = ufvk.GetOrchardKey();
        // Orchard notes will not have been selected if the UFVK does not contain an Orchard key.
        assert(fvk.has_value());
        return std::make_pair(
                fvk.value().ToInternalOutgoingViewingKey(),
                fvk.value().ToExternalOutgoingViewingKey());
    } else if (!spendable.saplingNoteEntries.empty()) {
        auto dfvk = ufvk.GetSaplingKey();
        // Sapling notes will not have been selected if the UFVK does not contain a Sapling key.
        assert(dfvk.has_value());
        return dfvk.value().GetOVKs();
    } else if (!spendable.utxos.empty()) {
        // Transparent UTXOs will not have been selected if the UFVK does not contain a transparent
        // key.
        auto tfvk = ufvk.GetTransparentKey();
        assert(tfvk.has_value());
        return tfvk.value().GetOVKsForShielding();
    } else {
        // This should be unreachable.
        throw std::runtime_error("No spendable inputs.");
    }
}

std::pair<uint256, uint256> WalletTxBuilder::SelectOVKs(
        const CWallet& wallet,
        const ZTXOSelector& selector,
        const SpendableInputs& spendable) const
{
    return examine(selector.GetPattern(), match {
        [&](const CKeyID& keyId) {
            return wallet.GetLegacyAccountKey().ToAccountPubKey().GetOVKsForShielding();
        },
        [&](const CScriptID& keyId) {
            return wallet.GetLegacyAccountKey().ToAccountPubKey().GetOVKsForShielding();
        },
        [&](const libzcash::SproutPaymentAddress&) {
            return wallet.GetLegacyAccountKey().ToAccountPubKey().GetOVKsForShielding();
        },
        [&](const libzcash::SproutViewingKey&) {
            return wallet.GetLegacyAccountKey().ToAccountPubKey().GetOVKsForShielding();
        },
        [&](const libzcash::SaplingPaymentAddress& addr) {
            libzcash::SaplingExtendedSpendingKey extsk;
            assert(wallet.GetSaplingExtendedSpendingKey(addr, extsk));
            return extsk.ToXFVK().GetOVKs();
        },
        [](const libzcash::SaplingExtendedFullViewingKey& sxfvk) {
            return sxfvk.GetOVKs();
        },
        [&](const UnifiedAddress& ua) {
            auto ufvk = wallet.GetUFVKForAddress(ua);
            // This is safe because spending key checks will have ensured that we have a UFVK
            // corresponding to this address.
            assert(ufvk.has_value());
            return GetOVKsForUFVK(ufvk.value().ToFullViewingKey(), spendable);
        },
        [&](const UnifiedFullViewingKey& ufvk) {
            return GetOVKsForUFVK(ufvk, spendable);
        },
        [&](const AccountZTXOPattern& acct) {
            if (acct.GetAccountId() == ZCASH_LEGACY_ACCOUNT) {
                return wallet.GetLegacyAccountKey().ToAccountPubKey().GetOVKsForShielding();
            } else {
                auto ufvk = wallet.GetUnifiedFullViewingKeyByAccount(acct.GetAccountId());
                // By definition, we have a UFVK for every known non-legacy account.
                assert(ufvk.has_value());
                return GetOVKsForUFVK(ufvk.value().ToFullViewingKey(), spendable);
            }
        },
    });
}

PrivacyPolicy TransactionEffects::GetRequiredPrivacyPolicy() const
{
    if (!spendable.utxos.empty()) {
        // TODO: Add a check for whether we need AllowLinkingAccountAddresses here. (#6467)
        if (payments.HasTransparentRecipient()) {
            // TODO: AllowFullyTransparent is the correct policy, but it’s a breaking change from
            //       previous behavior, so enable it separately. (#6409)
            // maxPrivacy = PrivacyPolicy::AllowFullyTransparent;
            return PrivacyPolicy::AllowRevealedSenders;
        } else {
            return PrivacyPolicy::AllowRevealedSenders;
        }
    } else if (payments.HasTransparentRecipient()) {
        return PrivacyPolicy::AllowRevealedRecipients;
    } else if (!spendable.orchardNoteMetadata.empty() && payments.HasSaplingRecipient()
               || !spendable.saplingNoteEntries.empty() && payments.HasOrchardRecipient()
               || !spendable.sproutNoteEntries.empty() && payments.HasSaplingRecipient()) {
        // TODO: This should only trigger when there is a non-zero valueBalance.
        return PrivacyPolicy::AllowRevealedAmounts;
    } else {
        return PrivacyPolicy::FullPrivacy;
    }
}

bool TransactionEffects::InvolvesOrchard() const
{
    return spendable.GetOrchardTotal() > 0 || payments.HasOrchardRecipient();
}

TransactionBuilderResult TransactionEffects::ApproveAndBuild(
        const Consensus::Params& consensus,
        const CWallet& wallet,
        const CChain& chain,
        const TransactionStrategy& strategy) const
{
    auto requiredPrivacy = this->GetRequiredPrivacyPolicy();
    if (!strategy.IsCompatibleWith(requiredPrivacy)) {
        return TransactionBuilderResult(strprintf(
            "The specified privacy policy, %s, does not permit the creation of "
            "the requested transaction. Select %s to allow this transaction "
            "to be constructed.",
            strategy.PolicyName(),
            TransactionStrategy::ToString(requiredPrivacy)
            + (requiredPrivacy == PrivacyPolicy::NoPrivacy ? "" : " or weaker")));
    }

    int nextBlockHeight = chain.Height() + 1;

    // Allow Orchard recipients by setting an Orchard anchor.
    std::optional<uint256> orchardAnchor;
    if (spendable.sproutNoteEntries.empty()
        && (InvolvesOrchard() || nPreferredTxVersion > ZIP225_MIN_TX_VERSION)
        && this->anchorConfirmations > 0)
    {
        LOCK(cs_main);
        auto anchorBlockIndex = chain[this->anchorHeight];
        assert(anchorBlockIndex != nullptr);
        orchardAnchor = anchorBlockIndex->hashFinalOrchardRoot;
    }

    auto builder = TransactionBuilder(consensus, nextBlockHeight, orchardAnchor, &wallet);
    builder.SetFee(fee);

    // Track the total of notes that we've added to the builder. This
    // shouldn't strictly be necessary, given `spendable.LimitToAmount`
    CAmount totalSpend = 0;

    // Create Sapling outpoints
    std::vector<SaplingOutPoint> saplingOutPoints;
    std::vector<SaplingNote> saplingNotes;
    std::vector<SaplingExtendedSpendingKey> saplingKeys;

    for (const auto& t : spendable.saplingNoteEntries) {
        saplingOutPoints.push_back(t.op);
        saplingNotes.push_back(t.note);

        libzcash::SaplingExtendedSpendingKey saplingKey;
        assert(wallet.GetSaplingExtendedSpendingKey(t.address, saplingKey));
        saplingKeys.push_back(saplingKey);

        totalSpend += t.note.value();
    }

    // Fetch Sapling anchor and witnesses, and Orchard Merkle paths.
    uint256 anchor;
    std::vector<std::optional<SaplingWitness>> witnesses;
    std::vector<std::pair<libzcash::OrchardSpendingKey, orchard::SpendInfo>> orchardSpendInfo;
    {
        LOCK(wallet.cs_wallet);
        if (!wallet.GetSaplingNoteWitnesses(saplingOutPoints, anchorConfirmations, witnesses, anchor)) {
            // This error should not appear once we're nAnchorConfirmations blocks past
            // Sapling activation.
            return TransactionBuilderResult("Insufficient Sapling witnesses.");
        }
        if (builder.GetOrchardAnchor().has_value()) {
            orchardSpendInfo = wallet.GetOrchardSpendInfo(spendable.orchardNoteMetadata, builder.GetOrchardAnchor().value());
        }
    }

    // Add Orchard spends
    for (size_t i = 0; i < orchardSpendInfo.size(); i++) {
        auto spendInfo = std::move(orchardSpendInfo[i]);
        if (!builder.AddOrchardSpend(
            std::move(spendInfo.first),
            std::move(spendInfo.second)))
        {
            return TransactionBuilderResult(
                strprintf("Failed to add Orchard note to transaction (check %s for details)", GetDebugLogPath())
            );
        } else {
            totalSpend += spendInfo.second.Value();
        }
    }

    // Add Sapling spends
    for (size_t i = 0; i < saplingNotes.size(); i++) {
        if (!witnesses[i]) {
            return TransactionBuilderResult(strprintf(
                "Missing witness for Sapling note at outpoint %s",
                spendable.saplingNoteEntries[i].op.ToString()
            ));
        }

        builder.AddSaplingSpend(saplingKeys[i].expsk, saplingNotes[i], anchor, witnesses[i].value());
    }

    // Add outputs
    for (const auto& r : payments.GetResolvedPayments()) {
        std::optional<TransactionBuilderResult> result;
        examine(r.address, match {
            [&](const CKeyID& keyId) {
                if (r.memo.has_value()) {
                    result = TransactionBuilderResult("Memos cannot be sent to transparent addresses.");
                } else {
                    builder.AddTransparentOutput(keyId, r.amount);
                }
            },
            [&](const CScriptID& scriptId) {
                if (r.memo.has_value()) {
                    result = TransactionBuilderResult("Memos cannot be sent to transparent addresses.");
                } else {
                    builder.AddTransparentOutput(scriptId, r.amount);
                }
            },
            [&](const libzcash::SaplingPaymentAddress& addr) {
                builder.AddSaplingOutput(
                        r.isInternal ? internalOVK : externalOVK, addr, r.amount,
                        r.memo.has_value() ? r.memo.value().ToBytes() : Memo::NoMemo().ToBytes());
            },
            [&](const libzcash::OrchardRawAddress& addr) {
                builder.AddOrchardOutput(
                        r.isInternal ? internalOVK : externalOVK, addr, r.amount,
                        r.memo.has_value() ? std::optional(r.memo.value().ToBytes()) : std::nullopt);
            },
        });
        if (result.has_value()) {
            return result.value();
        }
    }

    // Add transparent utxos
    for (const auto& out : spendable.utxos) {
        const CTxOut& txOut = out.tx->vout[out.i];
        builder.AddTransparentInput(COutPoint(out.tx->GetHash(), out.i), txOut.scriptPubKey, txOut.nValue);

        totalSpend += txOut.nValue;
    }

    // Find Sprout witnesses
    // When spending notes, take a snapshot of note witnesses and anchors as the treestate will
    // change upon arrival of new blocks which contain joinsplit transactions.  This is likely
    // to happen as creating a chained joinsplit transaction can take longer than the block interval.
    // So, we need to take locks on cs_main and wallet.cs_wallet so that the witnesses aren't
    // updated.
    //
    // TODO: these locks would ideally be shared for selection of Sapling anchors and witnesses
    // as well.
    std::vector<std::optional<SproutWitness>> vSproutWitnesses;
    {
        LOCK2(cs_main, wallet.cs_wallet);
        std::vector<JSOutPoint> vOutPoints;
        for (const auto& t : spendable.sproutNoteEntries) {
            vOutPoints.push_back(t.jsop);
        }

        // inputAnchor is not needed by builder.AddSproutInput as it is for Sapling.
        uint256 inputAnchor;
        if (!wallet.GetSproutNoteWitnesses(vOutPoints, anchorConfirmations, vSproutWitnesses, inputAnchor)) {
            // This error should not appear once we're nAnchorConfirmations blocks past
            // Sprout activation.
            return TransactionBuilderResult("Insufficient Sprout witnesses.");
        }
    }

    // Add Sprout spends
    for (int i = 0; i < spendable.sproutNoteEntries.size(); i++) {
        const auto& t = spendable.sproutNoteEntries[i];
        libzcash::SproutSpendingKey sk;
        assert(wallet.GetSproutSpendingKey(t.address, sk));

        builder.AddSproutInput(sk, t.note, vSproutWitnesses[i].value());

        totalSpend += t.note.value();
    }

    // TODO: We currently can’t store Sprout change in `Payments`, so we only validate the
    //       spend/output balance in the case that `TransactionBuilder` doesn’t need to
    //       (re)calculate the change. In future, we shouldn’t rely on `TransactionBuilder` ever
    //       calculating change.
    if (changeAddr.has_value()) {
        examine(changeAddr.value(), match {
            [&](const SproutPaymentAddress& addr) {
                builder.SendChangeToSprout(addr);
            },
            [&](const RecipientAddress&) {
                assert(totalSpend == payments.Total() + fee);
            }
        });
    }

    // Build the transaction
    return builder.Build();
}

// TODO: Lock Orchard notes (#6226)
void TransactionEffects::LockSpendable(CWallet& wallet) const
{
    LOCK2(cs_main, wallet.cs_wallet);
    for (auto utxo : spendable.utxos) {
        COutPoint outpt(utxo.tx->GetHash(), utxo.i);
        wallet.LockCoin(outpt);
    }
    for (auto note : spendable.sproutNoteEntries) {
        wallet.LockNote(note.jsop);
    }
    for (auto note : spendable.saplingNoteEntries) {
        wallet.LockNote(note.op);
    }
}

// TODO: Unlock Orchard notes (#6226)
void TransactionEffects::UnlockSpendable(CWallet& wallet) const
{
    LOCK2(cs_main, wallet.cs_wallet);
    for (auto utxo : spendable.utxos) {
        COutPoint outpt(utxo.tx->GetHash(), utxo.i);
        wallet.UnlockCoin(outpt);
    }
    for (auto note : spendable.sproutNoteEntries) {
        wallet.UnlockNote(note.jsop);
    }
    for (auto note : spendable.saplingNoteEntries) {
        wallet.UnlockNote(note.op);
    }
}
