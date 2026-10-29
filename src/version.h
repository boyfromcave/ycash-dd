// Copyright (c) 2012-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_VERSION_H
#define BITCOIN_VERSION_H

/**
 * network protocol versioning
 */

//! 270200 = the Vault-era protocol version (UPGRADE_VAULT.nProtocolVersion on
//! every network, the same on ycash6), taken with 6.22.0-rc1 (testnet plan T-8;
//! upgrade plan P8 part D). Before Vault activates it gates nobody: peers are
//! held to MIN_PEER_PROTO_VERSION below. Once Vault is the current epoch, a peer
//! below 270200 is disconnected. The live network speaks 270013 (Canopy era).
static const int PROTOCOL_VERSION = 270200;

//! initial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

//! In this version, 'getheaders' was introduced.
static const int GETHEADERS_VERSION = 31800;

//! disconnect from peers older than this proto version
static const int MIN_PEER_PROTO_VERSION = 170002;

//! nTime field added to CAddress, starting with this version;
//! if possible, avoid requesting addresses nodes older than this
static const int CADDR_TIME_VERSION = 31402;

//! BIP 0031, pong message, is enabled for all versions AFTER this one
static const int BIP0031_VERSION = 60000;

//! "mempool" command, enhanced "getdata" behavior starts with this version
static const int MEMPOOL_GD_VERSION = 60002;

//! "filter*" commands are disabled without NODE_BLOOM after and including this version
static const int NO_BLOOM_VERSION = 170004;

#endif // BITCOIN_VERSION_H
