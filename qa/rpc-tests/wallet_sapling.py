#!/usr/bin/env python3
# Copyright (c) 2018 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_coinbase_address,
    start_nodes,
    wait_and_assert_operationid_status,
    DEFAULT_FEE
)

from decimal import Decimal

# Test wallet behaviour with Sapling addresses
class WalletSaplingTest(BitcoinTestFramework):

    def setup_nodes(self):
        return start_nodes(self.num_nodes, self.options.tmpdir, extra_args=[[
            '-allowdeprecated=getnewaddress',
            '-allowdeprecated=z_getnewaddress',
            '-allowdeprecated=z_getbalance',
            '-allowdeprecated=z_gettotalbalance',
            '-allowdeprecated=z_listaddresses',
        ]] * self.num_nodes)

    def run_test(self):
        # Sanity-check the test harness
        assert_equal(self.nodes[0].getblockcount(), 200)

        taddr1 = self.nodes[1].getnewaddress()
        saplingAddr0 = self.nodes[0].z_getnewaddress('sapling')
        saplingAddr1 = self.nodes[1].z_getnewaddress('sapling')

        # Verify addresses
        assert(saplingAddr0 in self.nodes[0].z_listaddresses())
        assert(saplingAddr1 in self.nodes[1].z_listaddresses())
        assert_equal(self.nodes[0].z_validateaddress(saplingAddr0)['address_type'], 'sapling')
        assert_equal(self.nodes[0].z_validateaddress(saplingAddr1)['address_type'], 'sapling')

        # Verify balance
        assert_equal(self.nodes[0].z_getbalance(saplingAddr0), Decimal('0'))
        assert_equal(self.nodes[1].z_getbalance(saplingAddr1), Decimal('0'))
        assert_equal(self.nodes[1].z_getbalance(taddr1), Decimal('0'))

        # Node 0 shields some funds
        # taddr -> Sapling
        recipients = []
        recipients.append({"address": saplingAddr0, "amount": Decimal('10')})
        myopid = self.nodes[0].z_sendmany(get_coinbase_address(self.nodes[0]), recipients, 1, 0, 'AllowRevealedSenders')
        mytxid = wait_and_assert_operationid_status(self.nodes[0], myopid)

        self.sync_all()

        # Verify priority of tx is MAX_PRIORITY, defined as 1E+16 (10000000000000000)
        mempool = self.nodes[0].getrawmempool(True)
        assert(Decimal(mempool[mytxid]['startingpriority']) == Decimal('1E+16'))

        # Shield another coinbase UTXO
        myopid = self.nodes[0].z_sendmany(get_coinbase_address(self.nodes[0]), recipients, 1, 0, 'AllowRevealedSenders')
        mytxid = wait_and_assert_operationid_status(self.nodes[0], myopid)

        self.sync_all()
        self.nodes[2].generate(1)
        self.sync_all()

        # Verify balance
        assert_equal(self.nodes[0].z_getbalance(saplingAddr0), Decimal('20'))
        assert_equal(self.nodes[1].z_getbalance(saplingAddr1), Decimal('0'))
        assert_equal(self.nodes[1].z_getbalance(taddr1), Decimal('0'))

        # Node 0 sends some shielded funds to node 1
        # Sapling -> Sapling
        #         -> Sapling (change)
        recipients = []
        recipients.append({"address": saplingAddr1, "amount": Decimal('15')})
        myopid = self.nodes[0].z_sendmany(saplingAddr0, recipients, 1, 0)
        mytxid = wait_and_assert_operationid_status(self.nodes[0], myopid)

        self.sync_all()

        # Verify priority of tx is MAX_PRIORITY, defined as 1E+16 (10000000000000000)
        mempool = self.nodes[0].getrawmempool(True)
        assert(Decimal(mempool[mytxid]['startingpriority']) == Decimal('1E+16'))

        self.nodes[2].generate(1)
        self.sync_all()

        # Verify balance
        assert_equal(self.nodes[0].z_getbalance(saplingAddr0), Decimal('5'))
        assert_equal(self.nodes[1].z_getbalance(saplingAddr1), Decimal('15'))
        assert_equal(self.nodes[1].z_getbalance(taddr1), Decimal('0'))

        # Node 1 sends some shielded funds to node 0, as well as unshielding
        # Sapling -> Sapling
        #         -> taddr
        #         -> Sapling (change)
        recipients = []
        recipients.append({"address": saplingAddr0, "amount": Decimal('5')})
        recipients.append({"address": taddr1, "amount": Decimal('5')})
        myopid = self.nodes[1].z_sendmany(saplingAddr1, recipients, 1, 0, 'AllowRevealedRecipients')
        mytxid = wait_and_assert_operationid_status(self.nodes[1], myopid)

        self.sync_all()

        # Verify priority of tx is MAX_PRIORITY, defined as 1E+16 (10000000000000000)
        mempool = self.nodes[1].getrawmempool(True)
        assert(Decimal(mempool[mytxid]['startingpriority']) == Decimal('1E+16'))

        self.nodes[2].generate(1)
        self.sync_all()

        # Verify balance
        assert_equal(self.nodes[0].z_getbalance(saplingAddr0), Decimal('10'))
        assert_equal(self.nodes[1].z_getbalance(saplingAddr1), Decimal('5'))
        assert_equal(self.nodes[1].z_getbalance(taddr1), Decimal('5'))

        # Verify existence of Sapling related JSON fields
        resp = self.nodes[0].getrawtransaction(mytxid, 1)
        assert_equal(resp['valueBalance'], Decimal('5'))
        assert(len(resp['vShieldedSpend']) == 1)
        assert(len(resp['vShieldedOutput']) == 2)
        assert('bindingSig' in resp)
        shieldedSpend = resp['vShieldedSpend'][0]
        assert('cv' in shieldedSpend)
        assert('anchor' in shieldedSpend)
        assert('nullifier' in shieldedSpend)
        assert('rk' in shieldedSpend)
        assert('proof' in shieldedSpend)
        assert('spendAuthSig' in shieldedSpend)
        shieldedOutput = resp['vShieldedOutput'][0]
        assert('cv' in shieldedOutput)
        assert('cmu' in shieldedOutput)
        assert('ephemeralKey' in shieldedOutput)
        assert('encCiphertext' in shieldedOutput)
        assert('outCiphertext' in shieldedOutput)
        assert('proof' in shieldedOutput)

        # Verify importing a spending key will update the nullifiers and witnesses correctly
        sk0 = self.nodes[0].z_exportkey(saplingAddr0)
        saplingAddrInfo0 = self.nodes[2].z_importkey(sk0, "yes")
        assert_equal(saplingAddrInfo0["address_type"], "sapling")
        assert_equal(saplingAddrInfo0["address"], saplingAddr0)
        assert_equal(self.nodes[2].z_getbalance(saplingAddrInfo0["address"]), Decimal('10'))
        sk1 = self.nodes[1].z_exportkey(saplingAddr1)
        saplingAddrInfo1 = self.nodes[2].z_importkey(sk1, "yes")
        assert_equal(saplingAddrInfo1["address_type"], "sapling")
        assert_equal(saplingAddrInfo1["address"], saplingAddr1)
        assert_equal(self.nodes[2].z_getbalance(saplingAddrInfo1["address"]), Decimal('5'))

        # Verify importing a viewing key will update the nullifiers and witnesses correctly
        extfvk0 = self.nodes[0].z_exportviewingkey(saplingAddr0)
        saplingAddrInfo0 = self.nodes[3].z_importviewingkey(extfvk0, "yes")
        assert_equal(saplingAddrInfo0["address_type"], "sapling")
        assert_equal(saplingAddrInfo0["address"], saplingAddr0)
        assert_equal(self.nodes[3].z_getbalance(saplingAddrInfo0["address"]), Decimal('10'))
        extfvk1 = self.nodes[1].z_exportviewingkey(saplingAddr1)
        saplingAddrInfo1 = self.nodes[3].z_importviewingkey(extfvk1, "yes")
        assert_equal(saplingAddrInfo1["address_type"], "sapling")
        assert_equal(saplingAddrInfo1["address"], saplingAddr1)
        assert_equal(self.nodes[3].z_getbalance(saplingAddrInfo1["address"]), Decimal('5'))

        # Verify that z_gettotalbalance only includes watch-only addresses when requested
        assert_equal(self.nodes[3].z_gettotalbalance()['private'], '0.00')
        assert_equal(self.nodes[3].z_gettotalbalance(1, True)['private'], '15.00')

        # Make sure we get a useful error when trying to send to both sprout and sapling
        node4_sproutaddr = self.nodes[3].z_getnewaddress('sprout')
        node4_saplingaddr = self.nodes[3].z_getnewaddress('sapling')
        myopid = self.nodes[1].z_sendmany(
            taddr1,
            [{'address': node4_sproutaddr, 'amount': Decimal('2.5')},
             {'address': node4_saplingaddr, 'amount': Decimal('2.5') - DEFAULT_FEE}],
            1, DEFAULT_FEE, 'AllowRevealedSenders'
        )
        wait_and_assert_operationid_status(self.nodes[1], myopid, "failed", "Sending funds into the Sprout pool is no longer supported.")

if __name__ == '__main__':
    WalletSaplingTest().main()
