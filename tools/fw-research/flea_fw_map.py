#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline, hash-pinned BCM70015 firmware, ELF32 symbols and retained references."""

import argparse
from bisect import bisect_right
import hashlib
import json
import os
import re
import stat
import struct
import sys


BUNDLED_SHA256 = "8bf3a68f5c64686358a52274e40911a88c7f8c67ecbf6cf1557a49b4d7bc67c9"
BUNDLED_SIZE = 0xd3014
MAX_FIRMWARE_SIZE = 4 * 1024 * 1024  # include/crystalhd_ioctl_limits.h
TRAILER_SIZE = 20  # driver/linux/FleaDefs.h: length slot plus 16-byte CMAC
MAX_SYMBOL_RECORDS = 65536  # Aggregate across all tables/images, including duplicates.
MAX_STRING_TABLE_BYTES = MAX_FIRMWARE_SIZE
MAX_RELOCATION_RECORDS = 65536  # Includes no-ops and repeated tables/images.
MAX_OWNER_LOOKUP_STEPS = 1000000  # Bounds overlapping/aliased function intervals.
MAX_METADATA_OUTPUT_BYTES = 16 * 1024 * 1024  # Retained symbols and section names.
MAX_REFERENCE_OUTPUT_BYTES = 32 * 1024 * 1024  # Conservative JSON size accounting.
MAX_BOOTSTRAP_ANCHORS = 256  # Fixed, audited ARM instructions; never a general scan.
MAX_PICTURE_OUTPUT_ANCHORS = 204  # Separate fixed picture-path inventory, not a scan.
MAX_SCALER_FIR_REGIONS = 12
MAX_SCALER_FIR_BYTES = 2048
# Selected stock A32 bodies, literals and coefficient tables, not a device map.
_SCALER_FIR_REGIONS = (
    ("scaling_setup", 0x21ac, 716, "cc9fc5e53343bac1fa521854cc209f7a2d39ed406800d62e3f1f810d19edabe7"),
    ("scaling_dispatch", 0x1f8c, 544, "80d45cdf1110c163aff32f66ce053c2f6c938a8e2aa81691f4caee1071a315d7"),
    ("control_literals", 0x1f38, 84, "3c0851eae9ea6eb0130b95d93fb0df7a7b04c2611a47267df370009fbf23dcf9"),
    ("bank_literals", 0x244c, 24, "444c4ced61e1d13e8b594ea40369034b319ed0c12771d35379c8335eb5865264"),
    ("register_writer", 0x1e8e8, 12, "127fb56c30f3496c824443348ca74b9236add85c1381d8d0fae5bf61c0a9d927"),
    ("vertical_table", 0x2cdf0, 128, "6d77fc3ce84a6391ed6b30c21ddd525e21a54d38709662534668625c8fbb45a2"),
    ("horizontal_table", 0x2ccf0, 256, "a3cba1c64837a0c6dfd06b9c032bd605a0ac73e2dd7a316f00b0d51aaf1bf82a"),
    ("open_scaling_fields", 0x55d4, 156, "2235f36ad7d09336418f4d4571c4a761397529d4abb448b03fdd018573f2fbac"),
    ("picture_call_8518", 0x8518, 4, "c45be60a73538be6ea62105a869309a98a17942001e343b7fe3bfe95d17b5a64"),
    ("picture_call_8634", 0x8634, 4, "2d0d57c2380005c57f9c257fffdb7ea6f3c0e96a812d2e0aca79216d37d7e61e"))
MAX_ARC_COMMENT_BYTES = 4096
MAX_ARC_COMMENT_RECORDS = 128
MAX_ARC_METADATA_STRING_BYTES = 128
MAX_ARC_EXTENSION_BYTES = 112
MAX_ARC_EXTENSION_RECORDS = 10
MAX_ARC_METADATA_BYTES = 2 * (MAX_ARC_COMMENT_BYTES + MAX_ARC_EXTENSION_BYTES)
MAX_CSC_COMMAND_ANCHORS = 32  # Fixed local command path, not a dispatcher scan.
MAX_COMMAND_BUFFER_BRIDGE_REGIONS = 80
MAX_COMMAND_BUFFER_BRIDGE_BYTES = 40 * 1024
MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS = 2171
MAX_INNER_DESCRIPTOR_REGIONS = 48
MAX_INNER_DESCRIPTOR_BYTES = 4096
MAX_INNER_DISPATCH_REGIONS = 32
MAX_INNER_DISPATCH_BYTES = 4096
MAX_INNER_DISPATCH_RELOCATIONS = 64
# Complete selected code/table plus its original ELF ownership receipts.
# This is a conditional base-model dispatcher, not a vendor ISA decoder.
_INNER_DISPATCH_REGIONS = (
    ("inner_header", 0x79dd8, 52, "ba0c0bb760f1de121932856585ac9d8f15bdf2c4cf99473409eaa25006c9ab02"),
    ("section_2", 0xcea80, 40, "5794ebe11bc39a839b8758c911658c8567b44497bc64395c089a25fdcc567c77"),
    ("section_3", 0xceaa8, 40, "821702340e61bb1f94b2a4ab88be730bec8c3e06f838e7581b93f219c6134ba1"),
    ("section_4", 0xcead0, 40, "c405d1994af5b53637a49a1f6b0cd82b6d0d1823c5aadffb8516afca6e36c2d7"),
    ("section_15", 0xcec88, 40, "3d32cc7f94c9558edbacaac16df747a2ceecbca375ef4bcfe5f196759acc87f9"),
    ("section_18", 0xced00, 40, "52bc01729f0ef82c6a846aeee08b028581c4871ce94aa90bf0b6d8756eaf2958"),
    ("section_25", 0xcee18, 40, "9c04ce1900f898b52104d10d6a58ababb8e3715a91b42dbdb3ff434219fc2ff9"),
    ("section_47", 0xcf188, 40, "32c1f7e2e9001d6499e8e5f6adaf10c7c54c394e0f63263afdcea79b711bb874"),
    ("section_65", 0xcf458, 40, "4031ba9e8ef1d4954843bec4e668ce8e05cc688690897ec7523318aaf1a29ea9"),
    ("section_66", 0xcf480, 40, "0b5026352c2a92bee3fb12529019146bc141787a70abc5367d876bd3dae29d90"),
    ("section_69", 0xcf4f8, 40, "63e3ee452c1c646084e7455b715c393c21783cbea04a547326a9e3ab32334bdd"),
    ("core_code", 0x7a61c, 2172, "fae87bb7e239a8566f4aac20cf3207798b558c64d99bb161fab5aff77e02faa0"),
    ("core_relocations", 0xc9844, 636, "7ce105781f164014affad86d233e5b70d143225016e54aa67380e7e0acc8b52e"),
    ("prefix_dma_symbol", 0xc7b1c, 16, "8940fb41b14c4dc6c00c7a485a8146e0f8211e6b33bb1d02760999806b34de21"),
    ("prefix_dma_name", 0xc34af, 24, "bc24020c5929d40d07bd62daa3060f51bc6326f10cb34cde20978edb3ba9f859"),
    ("h264p_symbol", 0xc7e9c, 16, "01716c473e0f86487553be67b1f8ded3f21f34efc4558b9a321fdb7caee6c2ac"),
    ("h264p_name", 0xc3a4e, 20, "05fff25a856594a60b9266604f0aa1f10f8b89d572358d7ed4e57038e01bbe57"),
    ("h264_symbol", 0xc7c5c, 16, "ffa9ceff483a1a757cb9c7264705e5a2692e51fcbdd3287fe2c3b4244699b261"),
    ("h264_name", 0xc36ce, 24, "d7d9010847b630bd9ccebc7a92ae3292f975779451372c64adc5e0db5d958ea7"),
    ("mpeg_symbol", 0xc7ffc, 16, "75b9f505f3cdda277ff91be65423820c4e1f40437db4e24bd28f91ea4ba86aed"),
    ("mpeg_name", 0xc3bda, 24, "a2d0f727f0f9778a4905796a8e0a98c099e01969263420e863fa272be2410e94"),
    ("vc1_symbol", 0xc80cc, 16, "f7b9d4bd54f38fdc421b7eb7057060fc49ade50d52ba956131073af3179d5e0e"),
    ("vc1_name", 0xc3cd4, 23, "01addd91906748ec17d56571303131ea03ec7ac511eafd15edf61ef9099e6e84"),
    ("avs_symbol", 0xc8f6c, 16, "112c750ce372c305fbd801ff074bd1c36e589cdf7444c7c7d426a220c39dc1b5"),
    ("avs_name", 0xc4aca, 23, "f0ec8d1f5f67da694fd13980052b60ae686c5487ed9878fdc62856371fddd031"),
    ("mp4_symbol", 0xc8a2c, 16, "01116be15674a96da81b6e1ff6fe01334c5653d2bb30944a400467849f1f71b3"),
    ("mp4_name", 0xc454b, 23, "1c1df361c76477fa96f32dffb82938b49c108835a2148b218d5e6210f1379f1a"),
    ("h263_symbol", 0xc89ac, 16, "4ca780a94ec799b86373281f449f4224f41395559e65612550e6ec23853e59fd"),
    ("h263_name", 0xc44b6, 24, "ec1d1b3409777428d265ef4d88da5ca3889578fb218e7c5973c6f7aeace1a264"),
)
MAX_MFD_SOURCE_REGIONS = 10
MAX_MFD_SOURCE_BYTES = 1280
_ARM_PPB_HANDOFF_REGIONS = (
    ("acquire", 0xd624, 176, "e6ff28c676a32fe6219c53e6f40f2f23589e6f7f1f7235e4a46c95baf619907a"),
    ("peek", 0xd718, 104, "013bcc90b5e7d904f03f9787cc7e820978a711eba2fb37c95a63a7b8324c374d"),
    ("release", 0xd5a4, 128, "8276e18c409aa706a5b3e92b880887c4157256253d7be40312e28a7464b9c812"),
    ("translate", 0x1fdac, 192, "08d03815210fc1e069068765847cc4c5d847bd5df744f223f9851ee985e05d28"),
)
MAX_STOCK_HOST_COMMAND_REGIONS = 16
MAX_STOCK_HOST_COMMAND_BYTES = 16 * 1024
MAX_STOCK_HOST_COMMAND_CFG_STATES = 4096
MAX_STOCK_HOST_COMMAND_PARTITIONS = 256
MAX_PPB_BANK_REGIONS = 64
MAX_PPB_BANK_BYTES = 80 * 1024
MAX_PPB_BANK_RELOCATIONS = 2516
MAX_PPB_BANK_MODEL_STEPS = 4096
MAX_FRESH_INIT_REGIONS = 48
MAX_FRESH_INIT_BYTES = 16 * 1024
MAX_FRESH_INIT_AGGREGATE_BYTES = 80 * 1024
MAX_FRESH_INIT_ANCHORS = 160
MAX_FRESH_INIT_EVENTS = 64
MAX_INIT_REPLY_REGIONS = 12
MAX_INIT_REPLY_BYTES = 1024
MAX_INIT_REPLY_AGGREGATE_BYTES = 80 * 1024
MAX_INIT_REPLY_ANCHORS = 104
MAX_OPEN_REPLY_REGIONS = 48
MAX_OPEN_REPLY_BYTES = 14 * 1024
MAX_OPEN_REPLY_AGGREGATE_BYTES = 64 * 1024
MAX_OPEN_REPLY_INSTRUCTIONS = 640
MAX_OPEN_REPLY_ELF_RECORDS = 16
MAX_OPEN_REPLY_OWNED_RELOCATIONS = 80
MAX_OPEN_REPLY_SEMANTIC_RECEIPTS = 736
MAX_OPEN_REPLY_NEW_TABLE_RECORDS = 345
MAX_OPEN_REPLY_RELOCATION_RECORDS = 2516
# Complete selected bodies and metadata; LIMM bytes are inside their body pins.
_OPEN_REPLY_REGIONS = (
    ("host_open", 0x51c8, 2112, "d8ab1aaf4f635c38b66dcc210c6aed73f4f1bc73b485b1abf5f4adf8c1a35bdc"),
    ("channel_wrapper", 0x8a0, 760, "852df88584df06c581c4e32fec35d020233fd042a9c5a4579cdfbce26a535f12"),
    ("smp_open", 0xa2a4, 272, "152b7de5dac90649b746ae70ed0c9a8709c324785cbca4e56ef040cd02982338"),
    ("ordinary_decoder_open", 0xf7e4, 1032, "723911a2a94585093da0b4caebd2ba2a20b2c8bc87c4e91dd42646ae95a3a092"),
    ("open_packet_builder", 0x27480, 324, "c356d9c46eee35ea6dafb60e7e1ef49cf37c77726e4ba14e5e192f9cc2c50f91"),
    ("CmdChannelOpen", 0x4731c, 1068, "299ddffe5d7c502e0d0df9be965842c76f7de59cd09b30c65ed00fb807a49e22"),
    ("Core_ChanInitialize", 0x4928c, 636, "11aeb52ef9f2a3c8ab99cf099c75eade7a69abe991e3561a05f423e54b1b4d71"),
    ("Platform_DrvContextSize", 0x5de98, 8, "837494e6a5e29cf8ca42cf997921cc319edae871aebc286e75b44e30c933dd7b"),
    ("System_Activate", 0x34e34, 292, "9530baaafd18492433c0f9fdca77cfcd8d0ad1aa982e221303d6cd54de17521e"),
    ("Core_CopyDramToLsram", 0x34d18, 284, "a78e06a80f25a603981e888fa35ccf9e811dca10acdc4d71b919bd8902f88a1e"),
    ("Core_CircBuffer_Put", 0x363f8, 188, "b2b2c7a2b5d695d0e732f54fdc87cd1ced68f62814469793999d46cd518e9375"),
    ("Core_CircBuffer_Get", 0x2fbb4, 180, "4e0746e00040cedaee3d0275eaa7c1a14fa9f4dc056a60511dfd9b423a901a90"),
    ("Core_AttemptDisplay", 0x364b4, 1160, "7451b2344541e6428f8cf4e7da1df523acea2739bc40c4c881561e6441ca29c4"),
    ("Core_GetUndeliveredPPBs", 0x48cd8, 300, "7bd43689268dc47f4cddc6da57a25bad8e57db03f48fda4b67f5649cf4473797"),
    ("Platform_UpdateReleaseQueue", 0x36cdc, 56, "009ee74b4c5cb5684519e88dcda070e71a1df472a97ab19976761a57d184b5a4"),
    ("CmdChannelOpen_symbol", 0x69e60, 16, "7ca7d4b12ce4edb5ac69937833ba1cac67f921de37fd1a7448bf8e0a166a379d"),
    ("CmdChannelOpen_name", 0x67b15, 15, "a70af9b303345cb7a3bee0b3f23f26b1757b65faaa06c14b673bbecc76e3cd23"),
    ("Core_CircBuffer_Put_symbol", 0x69fd0, 16, "378217f0f198ef54d8a32c42229e75232a4325612c0dcb436aefc715b5624431"),
    ("Core_CircBuffer_Put_name", 0x67c7c, 20, "b3886b1ee538d44db9c30932c71ad01bc046c3f82649b6800cce747c7b94288c"),
    ("Core_GetUndeliveredPPBs_symbol", 0x6c030, 16, "489c3d066f146350b6cdc72a73b8af5fce33941483a832ff8f04ba070fab0c35"),
    ("Core_GetUndeliveredPPBs_name", 0x68b0f, 24, "e3f0ce39a361864e01669d0d186cf59b6258f438e9ae2e51fbeb61825f1b0d5e"),
    ("Core_CopyDramToLsram_symbol", 0x6c100, 16, "47f71315ca28d24515ae36f6ec130a0b5e87156a1696b1bbc1102630004b03a2"),
    ("Core_CopyDramToLsram_name", 0x68c06, 21, "877e6e345f80b8d612cae6b6594f197bc7cb45a1c7f38cafd3469e52db6b2bc2"),
    ("System_Activate_symbol", 0x6c110, 16, "5787f45f634723601c0bb273dbc1722baef803a36e7fb9ecf6a8231233ce7dd1"),
    ("System_Activate_name", 0x68c1b, 16, "5fb4a404d6d1b9463e064182640c757cdde1ccd9c3b5dc8d3b236114b60cb8fc"),
    ("Core_AttemptDisplay_symbol", 0x6c1a0, 16, "456ac9840b43973079c268e06cbb5e040b54908080bd87605d45bc76e1a5069b"),
    ("Core_AttemptDisplay_name", 0x68ccd, 20, "a474d0bfd5f5c154d186260bcded60e2ed959071e24d40e51c16e7b2ff959e48"),
    ("Core_ChanInitialize_symbol", 0x6c270, 16, "eb374b4daf7736bf59e41115b2513ffac54475ec9e6ed56819b3efc183b9d723"),
    ("Core_ChanInitialize_name", 0x68de3, 20, "62908ec357bb135ad3e91f6b7bb20552f9367ee0c37b635c435f45b9d3c4d856"),
    ("Core_CircBuffer_Get_symbol", 0x6c340, 16, "24771c2c2e3f0e3c71be480dd9f0e9527e8f0920367bb7bf40532b50eaa9798c"),
    ("Core_CircBuffer_Get_name", 0x68ea0, 20, "e7f04575cc0341cc9ccf63af12cafa4df5f2d666129958b63abbf13884bc2095"),
    ("dm_return_info_symbol", 0x6cd10, 16, "c1c140aaacc06fee8723cbe7b498aa307a98e005e61faf27a9d601c29911e525"),
    ("dm_return_info_name", 0x69835, 15, "f2ccf1e8a7700290fb61db81e8dbfa4bb9916be74f7a3f4fa5b9e3d072cd9f71"),
    ("Platform_UpdateReleaseQueue_symbol", 0x6cda0, 16, "c6f941d5a33eae54dfb98691de5fdf54fc3859f1474318fd0d814b762cfdb880"),
    ("Platform_UpdateReleaseQueue_name", 0x698ef, 28, "5264fa781e35b14d770f2465cdd54392d50633969bd77d81ad87f86741771d56"),
    ("Platform_DrvContextSize_symbol", 0x6cef0, 16, "ff6f634ee713269c7cd4ba177ef771e8177f928da7c94b9291c8d8340dc818d3"),
    ("Platform_DrvContextSize_name", 0x69aaa, 24, "54aaf3f80ae71bcd3ab9d78bc02ee7cc22a76b9c60b68c08ab325519c382b7c9"),
    ("picture_relocations", 0x6da34, 3132, "5c8ac2c91e08eeabe4567c2d908d2a7841c700b6a77c01b9329e727bf7eaf091"),
    ("slice_relocations", 0x6d020, 1008, "31d0234e0f31d3ed02cee74f82a66c64f897a69575888b83336f9fcabfe631a7"),
    ("open_command_literal", 0x27610, 4, "458262dafd94c17c450f767d215361a360801687865dba4ab3b243664c6c6597"),
    ("selected_open_dispatch", 0x62d8, 4, "c266cd49bb8474f561d8130cdcc7faf31d0d9e5921ac9b559c58b1f2e9ae8e0c"),
    ("slice_rela_header", 0x79b08, 40, "f0f411a72bc2b8bfeabf898110237d65e124e95052e801770aa1e009e2380cb5"),
    ("picture_rela_header", 0x79b58, 40, "e855a380af4559483720d27d271feb0ca488763d0be39328940d3d75c190d2ca"),
)
_OPEN_REPLY_ARM_SITES = (
    (0x898, 0xe51f01a4), (0x89c, 0xe12fff1e), (0x62d8, 0xebfffbba),
    (0x51c8, 0xe92d4ff0), (0x51cc, 0xe24dd01c), (0x51d8, 0xe3500000),
    (0x51dc, 0xa00000f), (0x51e0, 0xe2806014), (0x51e4, 0xe2804f45),
    (0x51e8, 0xe51fb0c8), (0x51ec, 0xe3e08000), (0x51f0, 0xe59b0000),
    (0x51f4, 0xe3500001), (0x51f8, 0xa00000d), (0x570c, 0xe51f05ec),
    (0x5710, 0xe5900004), (0x5714, 0xe0800105), (0x5718, 0xe59030cc),
    (0x571c, 0xe58d3000), (0x5720, 0xe59030c8), (0x5724, 0xe5d020d1),
    (0x5728, 0xe5d010d0), (0x572c, 0xe1a00007), (0x5730, 0xebffec5a),
    (0x5734, 0xe1a0b000), (0x5738, 0xe35b0000), (0x573c, 0xa000052),
    (0x5758, 0xe1a0000b), (0x575c, 0xeafffead), (0x5218, 0xe28dd01c),
    (0x521c, 0xe8bd8ff0), (0x8a0, 0xe92d47ff), (0x8a4, 0xe1a07000),
    (0x8a8, 0xe1a09001), (0x8ac, 0xe1a08002), (0x8b0, 0xebfffff8),
    (0x8b4, 0xe1a06000), (0x8b8, 0xe3560000), (0x8bc, 0xa00000f),
    (0x8c0, 0xe5960004), (0x8c4, 0xe28d2008), (0x8c8, 0xe1a01007),
    (0x8cc, 0xeb004801), (0x8d0, 0xe1a04000), (0x8d4, 0xe3540000),
    (0x8d8, 0xa00000a), (0x908, 0xe3a00073), (0x90c, 0xe1a02007),
    (0x910, 0xe28d3008), (0x914, 0xe0000097), (0x918, 0xe0865100),
    (0x91c, 0xe5960004), (0x920, 0xe2851014), (0x924, 0xeb0049f5),
    (0x928, 0xe1a04000), (0x92c, 0xe3540000), (0x930, 0xa000008),
    (0x970, 0xe28520ac), (0x974, 0xe1a03006), (0x978, 0xe1a01008),
    (0x97c, 0xe1a00007), (0x980, 0xeb00225e), (0x984, 0xe1a04000),
    (0x988, 0xe3540000), (0x98c, 0xa000048), (0xac0, 0xe2852028),
    (0xac4, 0xe1a01009), (0xac8, 0xe88d0044), (0xacc, 0xe28530ac),
    (0xad0, 0xe3a020e0), (0xad4, 0xe1a00007), (0xad8, 0xeb002317),
    (0xadc, 0xe1a04000), (0xae0, 0xe3540000), (0xae4, 0xa000008),
    (0xb0c, 0xe2851018), (0xb10, 0xe1a02006), (0xb14, 0xe1a00007),
    (0xb18, 0xeb0025e1), (0xb1c, 0xe1a04000), (0xb20, 0xe3540000),
    (0xb24, 0xa000010), (0xb40, 0xe1a00004), (0xb44, 0xeb007e1e),
    (0xb48, 0xe3a00000), (0xb4c, 0xe5c60740), (0xb58, 0xe1a03004),
    (0xb60, 0xeb008894), (0xb64, 0xe28dd010), (0xb68, 0xe8bd87f0),
    (0xb90, 0xe1a00004), (0xb94, 0xeafffff2), (0xa2a4, 0xe92d41f0),
    (0xa2a8, 0xe24dd038), (0xa2ac, 0xe1a07000), (0xa2b0, 0xe1a04001),
    (0xa2b4, 0xe1a06002), (0xa2b8, 0xe1a0200d), (0xa2bc, 0xe1a01007),
    (0xa2c0, 0xe5960008), (0xa2c4, 0xeb00128c), (0xa2c8, 0xe1a05000),
    (0xa2cc, 0xe3a08000), (0xa2d0, 0xe3550000), (0xa2d4, 0xa000008),
    (0xa2f0, 0xe1a00005), (0xa2f4, 0xeb005832), (0xa2f8, 0xea000012),
    (0xa2fc, 0xe5cd800d), (0xa300, 0xe3a00001), (0xa304, 0xe5cd0024),
    (0xa308, 0xe2841008), (0xa30c, 0xe5960008), (0xa310, 0xe1a0300d),
    (0xa314, 0xe1a02007), (0xa318, 0xeb001531), (0xa31c, 0xe1a05000),
    (0xa320, 0xe3550000), (0xa324, 0xa00000f), (0xa340, 0xe1a00005),
    (0xa344, 0xeb00581e), (0xa348, 0xe5c48000), (0xa354, 0xe1a03005),
    (0xa35c, 0xeb006295), (0xa360, 0xe28dd038), (0xa364, 0xe8bd81f0),
    (0xa370, 0xe5847004), (0xa374, 0xe584800c), (0xa378, 0xe3a01005),
    (0xa37c, 0xe5c41000), (0xa3ac, 0xe3a00000), (0xa3b0, 0xeaffffea),
    (0xf7e4, 0xe92d4ff0), (0xf7e8, 0xe24dd03c), (0xf7ec, 0xe1a05000),
    (0xf7f0, 0xe1a0a001), (0xf7f4, 0xe1a06002), (0xf7f8, 0xe1a07003),
    (0xf7fc, 0xe3a08000), (0xf800, 0xe3a04000), (0xf804, 0xe3a09000),
    (0xf808, 0xe5d5036c), (0xf80c, 0xe3500000), (0xf810, 0xa000008),
    (0xf830, 0xe28dd03c), (0xf834, 0xe8bd8ff0), (0xf838, 0xe3a00000),
    (0xf83c, 0xe58a0000), (0xf840, 0xe3560010), (0xf844, 0x3a000001),
    (0xf848, 0xe3a00002), (0xf84c, 0xeafffff7), (0xf850, 0xe595019c),
    (0xf854, 0xe7900106), (0xf858, 0xe3500000), (0xf85c, 0xa000008),
    (0xf87c, 0xe3a00002), (0xf880, 0xeaffffea), (0xf884, 0xe5d70020),
    (0xf888, 0xe3500001), (0xf88c, 0x1a000000), (0xf890, 0xe3a09001),
    (0xf894, 0xe3590000), (0xf898, 0xa00000b), (0xf8cc, 0xe30a084c),
    (0xf8d0, 0xeb00430e), (0xf8d4, 0xe1a04000), (0xf8d8, 0xe3540000),
    (0xf8dc, 0x1a000001), (0xf8e0, 0xe3a00003), (0xf8e4, 0xeaffffd1),
    (0xf980, 0xe5845064), (0xf984, 0xe5846000), (0xf9a4, 0xe3590000),
    (0xf9a8, 0xa000013), (0xfb08, 0xe3590000), (0xfb0c, 0xa000002),
    (0xfb10, 0xe5d502b4), (0xfb14, 0xe3500000), (0xfb18, 0x1a00001d),
    (0xfb1c, 0xe1c402d4), (0xfb20, 0xe1c422dc), (0xfb24, 0xe1cd02fc),
    (0xfb28, 0xe1cd23f4), (0xfb2c, 0xe1c401dc), (0xfb30, 0xe1c421d0),
    (0xfb34, 0xe1cd01fc), (0xfb38, 0xe1cd22f4), (0xfb3c, 0xe5940040),
    (0xfb40, 0xe5941034), (0xfb44, 0xe5942038), (0xfb48, 0xe5943018),
    (0xfb4c, 0xe1cd00fc), (0xfb50, 0xe1cd21f4), (0xfb54, 0xe5941008),
    (0xfb58, 0xe594200c), (0xfb5c, 0xe594303c), (0xfb60, 0xe88d000e),
    (0xfb64, 0xe5d430b1), (0xfb68, 0xe1a02009), (0xfb6c, 0xe1a01004),
    (0xfb70, 0xe1a00005), (0xfb74, 0xeb005e41), (0xfb78, 0xe1a08000),
    (0xfb7c, 0xe3580000), (0xfb80, 0xa000003), (0xfb84, 0xe1a00004),
    (0xfb88, 0xebfffed5), (0xfb8c, 0xe1a00008), (0xfb90, 0xeaffff26),
    (0xfba8, 0xe3a00001), (0xfbac, 0xe5c40230), (0xfbb0, 0xe58a4000),
    (0xfbb4, 0xe3590000), (0xfbb8, 0xa000001), (0xfbbc, 0xe5c502ac),
    (0xfbc0, 0xe58562b0), (0xfbc4, 0xe59a0000), (0xfbc8, 0xe595119c),
    (0xfbcc, 0xe7810106), (0xfbd0, 0xe1a01004), (0xfbd4, 0xe5950368),
    (0xfbd8, 0xeb00549b), (0xfbdc, 0xe1a00004), (0xfbe0, 0xebfffa59),
    (0xfbe4, 0xe3a00000), (0xfbe8, 0xeaffff10), (0x27480, 0xe92d4ff0),
    (0x27484, 0xe24ddf81), (0x27488, 0xe1a09000), (0x2748c, 0xe1a06001),
    (0x27490, 0xe1a0a002), (0x27494, 0xe1a0b003), (0x27498, 0xe3a00000),
    (0x2749c, 0xe58d0008), (0x274a0, 0xe28d4f42), (0x274a4, 0xe28d700c),
    (0x274a8, 0xe3a020fc), (0x274ac, 0xe3a01000), (0x274b0, 0xe1a00004),
    (0x274b4, 0xebffe48a), (0x274b8, 0xe3a020fc), (0x274bc, 0xe3a01000),
    (0x274c0, 0xe1a00007), (0x274c4, 0xebffe486), (0x274c8, 0xe1a05004),
    (0x274cc, 0xe1a08007), (0x274d0, 0xe59f0138), (0x274d4, 0xe5850000),
    (0x274d8, 0xe5960000), (0x274dc, 0xe5850004), (0x274e0, 0xe585a00c),
    (0x274e4, 0xe585b008), (0x274e8, 0xe59d0228), (0x274ec, 0xe5850010),
    (0x274f0, 0xe59d022c), (0x274f4, 0xe5850014), (0x274f8, 0xe59d0240),
    (0x274fc, 0xe5850018), (0x27500, 0xe59d0244), (0x27504, 0xe585001c),
    (0x27508, 0xe59d0248), (0x2750c, 0xe5850020), (0x27510, 0xe59d024c),
    (0x27514, 0xe5850024), (0x27518, 0xe59d0250), (0x2751c, 0xe5850028),
    (0x27520, 0xe59d0254), (0x27524, 0xe585002c), (0x27528, 0xe59d0258),
    (0x2752c, 0xe5850030), (0x27530, 0xe59d025c), (0x27534, 0xe5850038),
    (0x27538, 0xe59d0260), (0x2753c, 0xe585003c), (0x27540, 0xe59d0230),
    (0x27544, 0xe5850040), (0x27548, 0xe59d0234), (0x2754c, 0xe5850044),
    (0x27550, 0xe59d0238), (0x27554, 0xe5850048), (0x27558, 0xe59d023c),
    (0x2755c, 0xe585004c), (0x27560, 0xe3043e20), (0x27564, 0xe58d3000),
    (0x27568, 0xe1a03007), (0x2756c, 0xe1a02004), (0x27570, 0xe5991064),
    (0x27574, 0xe1a00009), (0x27578, 0xebfffeb7), (0x2757c, 0xe58d0008),
    (0x27580, 0xe5980008), (0x27584, 0xe5860044), (0x27588, 0xe598000c),
    (0x2758c, 0xe5860048), (0x27590, 0xe5980010), (0x27594, 0xe5860050),
    (0x27598, 0xe5980014), (0x2759c, 0xe58d0004), (0x275a0, 0xe2862058),
    (0x275a4, 0xe5990008), (0x275a8, 0xe59d1004), (0x275ac, 0xebffe1fe),
    (0x275b0, 0xe3a00001), (0x275b4, 0xe5c60220), (0x275b8, 0xe59d0008),
    (0x275bc, 0xe28ddf81), (0x275c0, 0xe8bd8ff0),
)
_OPEN_REPLY_ARC_SITES = (
    (16, 0x247c4, 0x62400000), (16, 0x25890, 0x20000500), (16, 0x258bc, 0x2ffdd920),
    (16, 0x258c0, 0x60079e00), (16, 0x258c4, 0x20001f00), (16, 0x24788, 0x100e3e04),
    (16, 0x2478c, 0x100e3600), (16, 0x24790, 0x636e3800), (16, 0x24794, 0x538e7e60),
    (16, 0x247c8, 0x800000c), (16, 0x247d0, 0x100d81fc), (16, 0x247d4, 0x18400e00),
    (16, 0x247d8, 0x8090008), (16, 0x247e0, 0x18200a00), (16, 0x247e4, 0x57e0fa03),
    (16, 0x247e8, 0x100d81f8), (16, 0x247ec, 0x200002a9), (16, 0x247f0, 0x61fffe02),
    (16, 0x247f4, 0x67e10500), (16, 0x247f8, 0x20000381), (16, 0x247fc, 0x57e0fa01),
    (16, 0x24800, 0x2000028c), (16, 0x24810, 0x20006c20), (16, 0x24814, 0x10091e04),
    (16, 0x24818, 0x8090010), (16, 0x2481c, 0x67e00100), (16, 0x24820, 0x20000c01),
    (16, 0x24824, 0x8090014), (16, 0x24828, 0x67e00100), (16, 0x2482c, 0x20000a81),
    (16, 0x24830, 0x8090024), (16, 0x24834, 0x67e00100), (16, 0x24838, 0x20000901),
    (16, 0x2483c, 0x8090028), (16, 0x24840, 0x67e00100), (16, 0x24844, 0x20000781),
    (16, 0x24848, 0x8090038), (16, 0x2484c, 0x67e00100), (16, 0x24850, 0x20000601),
    (16, 0x24854, 0x809003c), (16, 0x24858, 0x67e00100), (16, 0x2485c, 0x20000481),
    (16, 0x24860, 0x8090018), (16, 0x24864, 0x67e00100), (16, 0x24868, 0x20000301),
    (16, 0x2486c, 0x809001c), (16, 0x24870, 0x67e00100), (16, 0x24874, 0x20000181),
    (16, 0x24878, 0x8090020), (16, 0x2487c, 0x57e07a09), (16, 0x24880, 0x2000028e),
    (16, 0x24890, 0x20005c20), (16, 0x24894, 0x10091e04), (16, 0x248bc, 0x631f7c00),
    (16, 0x248c4, 0x80c0400), (16, 0x248c8, 0x67e00100), (16, 0x248d4, 0x20000202),
    (16, 0x248e0, 0x20005220), (16, 0x248e4, 0x10091e04), (16, 0x248e8, 0x9a90004),
    (16, 0x248ec, 0x57e6fa10), (16, 0x248f0, 0x40077f2c), (16, 0x248f4, 0x27ffe2a6),
    (16, 0x248f8, 0x60269a06), (16, 0x248fc, 0x402c7e0c), (16, 0x24900, 0x8006fe05),
    (16, 0x24904, 0x42608000), (16, 0x24908, 0x8498800), (16, 0x2490c, 0x67e17a07),
    (16, 0x24910, 0x20000281), (16, 0x24914, 0x40077f44), (16, 0x24918, 0x280575a0),
    (16, 0x2491c, 0x60269a00), (16, 0x24920, 0x20004a20), (16, 0x24924, 0x10091e04),
    (16, 0x24934, 0x50000000), (16, 0x24938, 0x10898000), (16, 0x2493c, 0x8006fe03),
    (16, 0x24940, 0x40207c00), (16, 0x24948, 0x50000000), (16, 0x2494c, 0x10008004),
    (16, 0x24950, 0x10009e00), (16, 0x2496c, 0xa090020), (16, 0x24a6c, 0x809003c),
    (16, 0x24a70, 0x8e90038), (16, 0x24a74, 0x100e0010), (16, 0x24a78, 0x8090048),
    (16, 0x24a7c, 0x829002c), (16, 0x24a80, 0x100e0014), (16, 0x24a84, 0x809004c),
    (16, 0x24a88, 0x100e1a1c), (16, 0x24a8c, 0x60482000), (16, 0x24a90, 0x606cb200),
    (16, 0x24a94, 0x100e0018), (16, 0x24a98, 0x6008a200), (16, 0x24a9c, 0x608b2c00),
    (16, 0x24aa0, 0x60abae00), (16, 0x24aa4, 0x28038a20), (16, 0x24aa8, 0x8cd81f4),
    (16, 0x24a3c, 0x57eafa00), (16, 0x24a40, 0x2000050a), (16, 0x24a4c, 0x601ffe07),
    (16, 0x24a50, 0x20002420), (16, 0x24a54, 0x10090004), (16, 0x24aac, 0x80d81f8),
    (16, 0x24ab0, 0x8298004), (16, 0x24abc, 0x4040fc00), (16, 0x24adc, 0x61df7c00),
    (16, 0x24b08, 0x50000000), (16, 0x24b0c, 0x10090004), (16, 0x24b10, 0x8210124),
    (16, 0x24b14, 0x10090208), (16, 0x24b18, 0x8010128), (16, 0x24b1c, 0x1009000c),
    (16, 0x24b20, 0x8010130), (16, 0x24b24, 0x40007c00), (16, 0x24b2c, 0x10090014),
    (16, 0x24b30, 0x60071c00), (16, 0x24b34, 0x10071e00), (16, 0x24b38, 0x10071e04),
    (16, 0x24b3c, 0x2fc107a0), (16, 0x24b40, 0x605ffe08), (16, 0x24b44, 0x2fc103a0),
    (16, 0x24b48, 0x81a6fe03), (16, 0x24b4c, 0x829000c), (16, 0x24b50, 0x10071e00),
    (16, 0x24b54, 0x10071e04), (16, 0x24b58, 0x60071c00), (16, 0x24b5c, 0x2fc103a0),
    (16, 0x24b60, 0x605ffe08), (16, 0x24b64, 0x2fc0ff80), (16, 0x24b68, 0x41a6fc00),
    (16, 0x24b70, 0x10091a10), (16, 0x24bac, 0x380f8020), (16, 0x24bb0, 0xb6e1060),
    (16, 0x266f8, 0x100e3e04), (16, 0x266fc, 0x100e3600), (16, 0x26700, 0x636e3800),
    (16, 0x26704, 0x538e7e38), (16, 0x2672c, 0x62600000), (16, 0x26734, 0x61c10400),
    (16, 0x26748, 0x42807c00), (16, 0x26760, 0x28297420), (16, 0x26764, 0x100e1a10),
    (16, 0x26768, 0x28297320), (16, 0x2676c, 0x41a02800), (16, 0x2677c, 0x88d801c),
    (16, 0x26780, 0x605f7c00), (16, 0x26788, 0x40017f08), (16, 0x26790, 0x80227e05),
    (16, 0x26794, 0x40000200), (16, 0x26798, 0x10002600), (16, 0x267a8, 0x4029fc00),
    (16, 0x267b0, 0x10009b30), (16, 0x267b8, 0x4049fc00), (16, 0x268c8, 0x8008130),
    (16, 0x268e0, 0x40007c00), (16, 0x268e8, 0x1001013c), (16, 0x26918, 0x8008130),
    (16, 0x26924, 0x40407c00), (16, 0x2692c, 0x10008524), (16, 0x26938, 0x40407c00),
    (16, 0x26958, 0x10008528), (16, 0x2696c, 0x380f8020), (16, 0x26970, 0xb6e1038),
    (16, 0x3b304, 0x380f8020), (16, 0x3b308, 0x401ffeec), (4, 0x9fa4, 0x61a00000),
    (4, 0x9fa8, 0x80007e05), (4, 0x9fb4, 0x41e07c00), (4, 0x9fbc, 0x8078010),
    (4, 0x9fc0, 0x605f7c00), (4, 0x9fc8, 0x61df7c00), (4, 0x9fd0, 0x2fffd420),
    (4, 0x9fd4, 0x40277e3c), (4, 0x9ea4, 0x62600000), (4, 0x9ea8, 0x62408200),
    (4, 0x9eac, 0x62210500), (4, 0x9efc, 0x2ff69920), (4, 0x9f00, 0x60469a00),
    (4, 0x9f18, 0x2ff678a0), (4, 0x9f1c, 0x60479e00), (4, 0x9f48, 0x2ff68300),
    (4, 0x9f5c, 0x2ff67020), (4, 0x9f60, 0x60469a00), (4, 0xb570, 0x62000000),
    (4, 0xb574, 0x61e08200), (4, 0xb578, 0x61bf7c00), (4, 0xb580, 0x60269a00),
    (4, 0xb584, 0x2ff3c820), (4, 0xb588, 0x605ffe08), (4, 0xb58c, 0x2ff3baa0),
    (4, 0xb590, 0x100e1c14), (4, 0xb594, 0x8268000), (4, 0xb598, 0x57e0fa02),
    (4, 0xb59c, 0x9a68004), (4, 0xb5a0, 0x200003ab), (4, 0xb5a4, 0x41c6fe01),
    (4, 0xb5a8, 0x57e0fa3f), (4, 0xb5ac, 0x20000209), (4, 0xb5b0, 0x57e6fa02),
    (4, 0xb5b4, 0x2000010b), (4, 0xb5b8, 0x57e6fa3f), (4, 0xb5bc, 0x2000028c),
    (4, 0xb5c8, 0x2837dfa0), (4, 0xb5cc, 0x60469a00), (4, 0xb5d0, 0x283d1080),
    (4, 0xb5d4, 0x57e77a40), (4, 0xb5d8, 0x61df7c01), (4, 0xb5e0, 0x8006fe02),
    (4, 0xb5e4, 0x40080000), (4, 0xb5e8, 0x10001e00), (4, 0xb5ec, 0x10081c04),
    (4, 0xb5f8, 0x50000000), (4, 0xb608, 0x380f8020), (4, 0xb60c, 0xb6e1020),
    (2, 0x4d10, 0x60200000), (2, 0x4d14, 0x607f7c00), (2, 0x4d1c, 0x605f7c00),
    (2, 0x4d24, 0x8014040), (2, 0x4d28, 0x60007e0c), (2, 0x4d2c, 0x57e07a0c),
    (2, 0x4d30, 0x27fffe01), (2, 0x4d34, 0x8014040), (2, 0x4d38, 0x67e07a04),
    (2, 0x4d3c, 0x200002a2), (2, 0x4d40, 0x601ffe08), (2, 0x4d44, 0x14010220),
    (2, 0x4d48, 0x14010624), (2, 0x4d4c, 0x14010028), (2, 0x4d50, 0x20000180),
    (2, 0x4d54, 0x14010230), (2, 0x4d58, 0x14010634), (2, 0x4d5c, 0x14010038),
    (2, 0x4d60, 0x8014040), (2, 0x4d64, 0x67e07a0f), (2, 0x4d68, 0x27fffe82),
    (2, 0x4d6c, 0x8018004), (2, 0x4d70, 0x8418000), (2, 0x4d74, 0x57e10100),
    (2, 0x4d78, 0x20000102), (2, 0x4d7c, 0x20000820), (2, 0x4d80, 0x50000000),
    (2, 0x4d84, 0x57e17a02), (2, 0x4d88, 0x27fffe0b), (2, 0x4d8c, 0x57e17a3f),
    (2, 0x4d90, 0x27fffd09), (2, 0x4d94, 0x57e07a02), (2, 0x4d98, 0x27fffc0b),
    (2, 0x4d9c, 0x57e07a3f), (2, 0x4da0, 0x27fffb09), (2, 0x4da4, 0x80017e02),
    (2, 0x4da8, 0x8000), (2, 0x4dac, 0x40417e01), (2, 0x4db0, 0x57e17a40),
    (2, 0x4db4, 0x605f7c01), (2, 0x4dbc, 0x10008400), (2, 0x4dc0, 0x380f8000),
    (4, 0xb650, 0x62ff7c00), (4, 0xb658, 0x61ff7c00), (4, 0xb688, 0x62bf7c00),
    (4, 0xb818, 0x61df7c00), (4, 0xb880, 0x42669c00), (4, 0xb8d8, 0xac985f6),
    (4, 0xb8dc, 0xa470178), (4, 0xb8e0, 0x600bae00), (4, 0xb8e4, 0x828b7e03),
    (4, 0xb8e8, 0x528a2c00), (4, 0xb8ec, 0x828a7e03), (4, 0xb8f0, 0x428a2c00),
    (4, 0xb8f4, 0x828a7e02), (4, 0xb8f8, 0x40292800), (4, 0xb8fc, 0x2ff34fa0),
    (4, 0xb900, 0x605ffee4), (4, 0xb904, 0x2ff34b80), (4, 0xb944, 0x1fe88d00),
    (4, 0xb948, 0x20000483), (4, 0xb970, 0x80a81b4), (4, 0xb974, 0x40007e01),
    (4, 0xb978, 0x100a81b4), (4, 0xb97c, 0x80a8160), (4, 0xb980, 0x2fff7a20),
    (4, 0xb984, 0x40292800), (4, 0xb988, 0x28008c00), (16, 0x26158, 0x61bf7c00),
    (16, 0x26170, 0x2fbd73a0), (16, 0x26174, 0x8068160), (16, 0x26178, 0x67e00100),
    (16, 0x2617c, 0x20000c01), (16, 0x26180, 0x605f7c00), (16, 0x26188, 0x8610178),
    (16, 0x2618c, 0x50208200), (16, 0x26190, 0x57e00700), (16, 0x26194, 0x20000281),
    (16, 0x26198, 0x4020fe01), (16, 0x2619c, 0x57e0fa22), (16, 0x261a0, 0x27fffdab),
    (16, 0x261a4, 0x4061fee4), (16, 0x261a8, 0x603fffff), (16, 0x261ac, 0x57e0fa00),
    (16, 0x261d4, 0x2fc5d4a0), (16, 0x261d8, 0x60008200), (16, 0x261e0, 0x2fbd65a0),
    (16, 0x261e4, 0x8068164), (16, 0x261e8, 0x67e00100), (16, 0x261ec, 0x20000981),
    (16, 0x261f0, 0x607f7c00), (16, 0x261f8, 0x8418178), (16, 0x261fc, 0x50208200),
    (16, 0x26200, 0x679ffe22), (16, 0x26204, 0x30000200), (16, 0x26208, 0x57e00500),
    (16, 0x2620c, 0x20000181), (16, 0x26210, 0x40417ee4), (16, 0x26214, 0x4020fe01),
    (16, 0x26218, 0x603fffff), (16, 0x26264, 0x2fc5c2a0), (16, 0x26268, 0x60008200),
    (4, 0xbe38, 0x80207e05), (4, 0xbe3c, 0x4040fc00), (4, 0xbe44, 0x821040f),
    (4, 0xbe48, 0x57e0faff), (4, 0xbe4c, 0x380f8001), (4, 0xbe50, 0x8210010),
    (4, 0xbe54, 0x80007e03), (4, 0xbe58, 0x7c00), (4, 0xbe60, 0x20fc00),
    (4, 0xbe68, 0x380f8020), (4, 0xbe6c, 0x10008004),
)
# Name, index, section, VA, size, symbol record, name bytes.
_OPEN_REPLY_SYMBOLS = (
    ("CmdChannelOpen", 47, 16, 0x24788, 1068, 0x69e60, 0x67b15),
    ("Core_CircBuffer_Put", 70, 4, 0xb554, 188, 0x69fd0, 0x67c7c),
    ("Core_GetUndeliveredPPBs", 588, 16, 0x26144, 300, 0x6c030, 0x68b0f),
    ("Core_CopyDramToLsram", 601, 4, 0x9e74, 284, 0x6c100, 0x68c06),
    ("System_Activate", 602, 4, 0x9f90, 292, 0x6c110, 0x68c1b),
    ("Core_AttemptDisplay", 611, 4, 0xb610, 1160, 0x6c1a0, 0x68ccd),
    ("Core_ChanInitialize", 624, 16, 0x266f8, 636, 0x6c270, 0x68de3),
    ("Core_CircBuffer_Get", 637, 2, 0x4d10, 180, 0x6c340, 0x68ea0),
    ("dm_return_info", 794, 21, 0x78620, 128, 0x6cd10, 0x69835),
    ("Platform_UpdateReleaseQueue", 803, 4, 0xbe38, 56, 0x6cda0, 0x698ef),
    ("Platform_DrvContextSize", 824, 16, 0x3b304, 8, 0x6cef0, 0x69aaa),
)
# Owner, record position, source VA, vendor type, symbol index, signed addend.
_OPEN_REPLY_OWNED_RELOCATIONS = (
    ("CmdChannelOpen", 0x729b4, 0x24808, 4, 19, 500), ("CmdChannelOpen", 0x729c0, 0x2480c, 6, 627, 0),
    ("CmdChannelOpen", 0x729cc, 0x24888, 4, 19, 520), ("CmdChannelOpen", 0x729d8, 0x2488c, 6, 537, 0),
    ("CmdChannelOpen", 0x729e4, 0x248b4, 4, 19, 552), ("CmdChannelOpen", 0x729f0, 0x248c0, 4, 622, 0),
    ("CmdChannelOpen", 0x729fc, 0x248d0, 4, 19, 816), ("CmdChannelOpen", 0x72a08, 0x248d8, 6, 627, 0),
    ("CmdChannelOpen", 0x72a14, 0x24918, 6, 627, 0), ("CmdChannelOpen", 0x72a20, 0x2492c, 6, 627, 0),
    ("CmdChannelOpen", 0x72a2c, 0x24944, 4, 794, 0), ("CmdChannelOpen", 0x72a38, 0x24974, 6, 627, 0),
    ("CmdChannelOpen", 0x72a44, 0x249dc, 6, 627, 0), ("CmdChannelOpen", 0x72a50, 0x24a14, 6, 824, 0),
    ("CmdChannelOpen", 0x72a5c, 0x24a20, 6, 824, 0), ("CmdChannelOpen", 0x72a68, 0x24a44, 6, 627, 0),
    ("CmdChannelOpen", 0x72a74, 0x24aa4, 6, 624, 0), ("CmdChannelOpen", 0x72a80, 0x24b3c, 6, 645, 0),
    ("CmdChannelOpen", 0x72a8c, 0x24b44, 6, 644, 0), ("CmdChannelOpen", 0x72a98, 0x24b5c, 6, 645, 0),
    ("CmdChannelOpen", 0x72aa4, 0x24b64, 6, 644, 0), ("CmdChannelOpen", 0x72ab0, 0x24b6c, 4, 794, 0),
    ("Core_CircBuffer_Put", 0x6e478, 0xb584, 6, 646, 0), ("Core_CircBuffer_Put", 0x6e484, 0xb58c, 6, 644, 0),
    ("Core_CircBuffer_Put", 0x6e490, 0xb5c4, 4, 19, 1884), ("Core_CircBuffer_Put", 0x6e49c, 0xb5c8, 6, 627, 0),
    ("Core_CircBuffer_Put", 0x6e4a8, 0xb5d0, 6, 629, 0), ("Core_GetUndeliveredPPBs", 0x73164, 0x2615c, 4, 621, 1536),
    ("Core_GetUndeliveredPPBs", 0x73170, 0x26168, 4, 19, 1840), ("Core_GetUndeliveredPPBs", 0x7317c, 0x2616c, 6, 627, 0),
    ("Core_GetUndeliveredPPBs", 0x73188, 0x26170, 6, 637, 0), ("Core_GetUndeliveredPPBs", 0x73194, 0x26184, 4, 621, 1024),
    ("Core_GetUndeliveredPPBs", 0x731a0, 0x261d4, 6, 578, 0), ("Core_GetUndeliveredPPBs", 0x731ac, 0x261e0, 6, 637, 0),
    ("Core_GetUndeliveredPPBs", 0x731b8, 0x261f4, 4, 621, 1024), ("Core_GetUndeliveredPPBs", 0x731c4, 0x26230, 4, 19, 1840),
    ("Core_GetUndeliveredPPBs", 0x731d0, 0x26234, 6, 627, 0), ("Core_GetUndeliveredPPBs", 0x731dc, 0x26264, 6, 578, 0),
    ("Core_CopyDramToLsram", 0x6dfec, 0x9eec, 6, 644, 0), ("Core_CopyDramToLsram", 0x6dff8, 0x9efc, 6, 646, 0),
    ("Core_CopyDramToLsram", 0x6e004, 0x9f18, 6, 641, 0), ("Core_CopyDramToLsram", 0x6e010, 0x9f48, 6, 644, 0),
    ("Core_CopyDramToLsram", 0x6e01c, 0x9f5c, 6, 641, 0), ("System_Activate", 0x6e028, 0x9fb8, 4, 622, 0),
    ("System_Activate", 0x6e034, 0x9fcc, 4, 621, 0), ("System_Activate", 0x6e040, 0xa02c, 6, 669, 0),
    ("System_Activate", 0x6e04c, 0xa038, 6, 687, 0), ("System_Activate", 0x6e058, 0xa044, 6, 704, 0),
    ("System_Activate", 0x6e064, 0xa050, 6, 727, 0), ("System_Activate", 0x6e070, 0xa05c, 6, 741, 0),
    ("System_Activate", 0x6e07c, 0xa068, 6, 773, 0), ("System_Activate", 0x6e088, 0xa074, 6, 748, 0),
    ("System_Activate", 0x6e094, 0xa07c, 6, 823, 0), ("Core_AttemptDisplay", 0x6e4b4, 0xb648, 4, 621, 512),
    ("Core_AttemptDisplay", 0x6e4c0, 0xb65c, 4, 621, 0), ("Core_AttemptDisplay", 0x6e4cc, 0xb68c, 4, 621, 1536),
    ("Core_AttemptDisplay", 0x6e4d8, 0xb6b8, 6, 646, 0), ("Core_AttemptDisplay", 0x6e4e4, 0xb6c0, 6, 644, 0),
    ("Core_AttemptDisplay", 0x6e4f0, 0xb6e8, 6, 646, 0), ("Core_AttemptDisplay", 0x6e4fc, 0xb6f0, 6, 644, 0),
    ("Core_AttemptDisplay", 0x6e508, 0xb724, 4, 621, 1024), ("Core_AttemptDisplay", 0x6e514, 0xb81c, 4, 621, 1024),
    ("Core_AttemptDisplay", 0x6e520, 0xb8fc, 6, 645, 0), ("Core_AttemptDisplay", 0x6e52c, 0xb904, 6, 644, 0),
    ("Core_AttemptDisplay", 0x6e538, 0xb938, 6, 645, 0), ("Core_AttemptDisplay", 0x6e544, 0xb940, 6, 644, 0),
    ("Core_AttemptDisplay", 0x6e550, 0xb988, 6, 802, 0), ("Core_AttemptDisplay", 0x6e55c, 0xba4c, 4, 19, 1920),
    ("Core_AttemptDisplay", 0x6e568, 0xba50, 6, 627, 0), ("Core_AttemptDisplay", 0x6e574, 0xba58, 6, 626, 0),
    ("Core_ChanInitialize", 0x733bc, 0x26758, 6, 604, 0), ("Core_ChanInitialize", 0x733c8, 0x26760, 6, 824, 0),
    ("Core_ChanInitialize", 0x733d4, 0x26768, 6, 824, 0), ("Core_ChanInitialize", 0x733e0, 0x26774, 6, 604, 0),
    ("Core_ChanInitialize", 0x733ec, 0x26784, 4, 27, 1792), ("Platform_UpdateReleaseQueue", 0x6e5d4, 0xbe40, 4, 622, 0),
    ("Platform_UpdateReleaseQueue", 0x6e5e0, 0xbe5c, 4, 794, 0),
)
_INIT_REPLY_REGIONS = (
    ("delivery_relocation", 0x72948, 12, "47550cc0eb8d0d6d588885855ad7ef12daa7ff9bb1a6b903900f77300181381e"),
    ("delivery_symbol", 0x6cd00, 16, "d5051f32cf622bb3fed31ff962a2b2787272a099ca3cb1f0f75a71a5cac9b20f"),
    ("delivery_name", 0x69824, 17, "eaf35101f02f49b3f084841067167b1382adac2948b7afe528f1a83e943ab403"),
    ("delivery_section", 0x79888, 40, "dadf59bee18295ef7f9bf1c67104d066c2c0d8a0b150511b07af868c0026c8b5"),
)
_INIT_REPLY_ARM_SITES = (
    (0x28058, 0xe1a04000), (0x280ac, 0xe28d301c), (0x27be0, 0xe1a0a003),
    (0x27c7c, 0xe59d000c),
    (0x280a4, 0xe59421b0), (0x27bd0, 0xe92d4fff), (0x27bd4, 0xe24dd01c),
    (0x27c80, 0xe590000c), (0x27c84, 0xe58d0008), (0x27d48, 0xe59d0024),
    (0x27d4c, 0xe59d1008), (0x27d50, 0xe0800001), (0x27d54, 0xe58a0000),
    (0x27d5c, 0xe59a2000), (0x27d60, 0xe1cd20f0), (0x27d64, 0xe3a03000),
    (0x2af2c, 0xe92d4fff), (0x2af30, 0xe24dd014), (0x2af3c, 0xe1a09003),
    (0x2afac, 0xe5849004), (0x2afb0, 0xe59d0048), (0x2afb4, 0xe5840008),
    (0x2a48c, 0xe0813101), (0x2a490, 0xe2804040), (0x2a494, 0xe0843183),
    (0x2a498, 0xe593200c), (0x2a49c, 0xe3520000), (0x2a4a0, 0x1a000006),
    (0x2a4c0, 0xe3520203), (0x2a4c4, 0x3a000000), (0x2a4cc, 0xe5903004),
    (0x2a4d0, 0xe1530002), (0x2a4d4, 0x9a000000), (0x2a4e8, 0xe5933008),
    (0x2a4ec, 0xe3130004), (0x2a4f0, 0x0a000002), (0x2a500, 0xe5903004),
    (0x2a504, 0xe0423003), (0x2a508, 0xe5904008), (0x2a50c, 0xe0832004),
    (0x2a510, 0xe2803d61), (0x2a514, 0xe7832101),
    (0x2a4dc, 0xe0813101), (0x2a4e0, 0xe2804040), (0x2a4e4, 0xe0843183),
    (0x2a428, 0xe1d050be), (0x2a42c, 0xe0855105), (0x2a430, 0xe2816040),
    (0x2a434, 0xe0864185),
    (0x2a438, 0xe5905004), (0x2a43c, 0xe594600c), (0x2a440, 0xe0453006),
    (0x2a444, 0xe1d050be), (0x2a448, 0xe2816d61), (0x2a44c, 0xe7965105),
    (0x2a450, 0xe0855003), (0x2a454, 0xe5805004),
    (0x271d0, 0xe1a04000), (0x271e4, 0xe28d8004), (0x2720c, 0xe1a07008),
    (0x27244, 0xe28d3004), (0x273bc, 0xe597100c), (0x273c0, 0xe594000c),
    (0x273c4, 0xe2842e25), (0x273c8, 0xebffe277), (0x273cc, 0xe28f0f81),
    (0x273d4, 0xe5971010), (0x273d8, 0xe594000c), (0x273dc, 0xe2842f95),
    (0x273e0, 0xebffe271), (0x273e4, 0xe5970008), (0x273e8, 0xe584018c),
    (0x1fdb0, 0xe1a03000), (0x1fdb4, 0xe1a04001), (0x1fdbc, 0xe5910028),
    (0x1fdc0, 0xe0800004), (0x1fdc4, 0xe5915030), (0x1fdc8, 0xe0400005),
    (0x1fdcc, 0xe5820000), (0x1fdd0, 0xe5920000), (0x1fdd4, 0xe5915018),
    (0x1fdd8, 0xe1500005), (0x1fddc, 0x3a000004), (0x1fde0, 0xe5920000),
    (0x1fde4, 0xe591501c), (0x1fde8, 0xe1500005), (0x1fdec, 0x8a000000),
    (0x1fdf0, 0xea00001a), (0x1fdf4, 0xe5930004), (0x1fdf8, 0xe3500000),
    (0x1fdfc, 0x0a000015), (0x1fe58, 0xe3a00002), (0x1fe64, 0xe3a00000),
    (0x1fdb8, 0xe1a01003), (0x1fdac, 0xe92d4030), (0x1fe5c, 0xe8bd8030),
    (0x1fe68, 0xeafffffb),
)
_INIT_REPLY_ARC_SITES = (
    (0x258b4, 0x60079e00), (0x24604, 0x61c00000),
    (0x246b4, 0x601f7c00), (0x246bc, 0x1007000c),
    (0x246c0, 0x40007e0c), (0x246c4, 0x10070010),
)
# Additional complete bodies/data for the private, conditional INIT receipt.
# The separately bounded command-buffer bridge is charged in full as well.
_FRESH_INIT_REGIONS = (
    ("host_init", 0x5ccc, 0x260, "9e5582a78d61ca8ceddc0fc6b5e59bf7eda66238837131383bec1d2a4c109428"),
    ("init_context", 0x54c, 0x354, "766ebdef10b26af75d180b82e886deaa7a35746c8b13748c4705fea13e803b4f"),
    ("controller_factory", 0xe8b0, 0x44c, "5a39a3cd8e72b709a02706b0d432eabb56aa7488d90929fd5330ab6d0bd14c45"),
    ("image_initialize", 0x26658, 0x98, "01c6e5189f63daaa0ac6dee59d7b429c81fe644cb4a33b72fab8ba7c626a9555"),
    ("image_load_call", 0x26358, 0xa0, "b2c2f4ee7c5d8af12536d9c727de895e9afea56adb7c71d651e09bc04e10b5c5"),
    ("init_packet_builder", 0x271c8, 0x234, "109eabdb9d6276f1d0279c69d6792cad22a095570f28889ebb6b5523341ba26e"),
    ("transport", 0x2705c, 0x16c, "bd461670f479a8e1f005d75357912eee0b0b61d875c6c87c7a10f79d9303d6f8"),
    ("register_access", 0x25010, 0x24, "c08d4a86aa8353d7e2000f5eb14ebca387b3ebdfd46aa951902c54137acf7599"),
    ("event_helpers", 0x2052c, 0x17c, "867160722e72d59fe8b305152f536db655f45a368ab31dd31e9d2e8efc37503e"),
    ("response_callback", 0x2c16c, 0x24, "959a33dc5e3a95e8dac82cc6652c5de37559cfa1a01383387f7aee721c3fd8ab"),
    ("response_registration", 0x28194, 0xb8, "32b6a70c0765cbb0dd64f0aed34f791046acc9c7593ff81dd6d83e9bfe7b59c0"),
    ("irq_slot_registration", 0x6ea0, 0x34, "9e6b69c108f5f1af4178bd069f394176b9675f22e5720791fe13c24e126e6a8f"),
    ("irq_dispatch", 0x6ef0, 0x100, "fd586631b4c4ff1f065091fc0f186c347b8ec40153b707fb222b0b08abc69bd8"),
    ("irq_vector", 0x0, 0x3c, "de8b9454a35d359a236ce99751fa0914d415392a41e1b32d7cb903168d5155e0"),
    ("irq_entry", 0xdc, 0x1c, "2deff474b41a300c7da7ebe19e02683f29533be104ae9a8cfc9ad0861d2008c3"),
    ("irq_enable", 0xad44, 0xc, "639dfcd5a4d39cf151d135ade70d4229d94cd4ab08b590560d3208b3082a8b76"),
    ("registration_literals", 0x28694, 0x8, "4d15ccc11ebc9eb4cabcbcd590b6a7bce792c2286027b77b994e423113c148cf"),
    ("irq_dispatch_literals", 0x7128, 0xc, "27a79d882ee97dda06f4c2ed55e0f598e36dbb0c56cd990ed09e64a792176da1"),
    ("base_initializer", 0x7534, 0x70, "1ef4bebb542793521b81d76f3625db33c657dcde88d5a77cb070de87b62877ff"),
    ("base_constructor", 0x1e87c, 0x28, "644ac03f556102511f554a1eeaf73e1ea41e2f7ffe8daca066dffa26fb0c4075"),
    ("base_constructor_arguments", 0x264, 0x10, "5c4f2da001f1dad1c8e03da7bb19fd45ea129c3253ed03a40051624f25b05a7b"),
    ("register_table", 0xcfc04, 0xec, "2c14f6c782aabd732ff329b7fea0f691769d6643fe07188a072ecb61daa45881"),
    ("register_table_copy", 0x26dd8, 0xa4, "f9e5ec75c0e6614253ac2a3359a48602641ba7e7c3726695ad11c570ff54099e"),
    ("shortcut_global_literal", 0x5128, 0x4, "90235ef9116585e3b06f150c19cc273c8b4098db381d46cf3454ab59501e3688"),
    ("return_logging", 0x22db8, 0x3c, "5a09c7179f12d8905659b211b1b9f3c27fb37813587ac5770f5a9128e8c87020"),
    ("outer_init", 0x47180, 0x19c, "f6a261cb43d156ce01a7a04419811312e5750c4271ff1f99f6c4f6f17cb34c72"),
    ("outer_loop", 0x491e0, 0xac, "f48507026c72adadfd335678642b966e78fbe632cf3ab057de48c8f62fba4358"),
    ("outer_local_clear", 0x30160, 0x24, "dc0016171f31d69a69d50c4d67559ae5756ea8195e168958555a48fcf413ea7c"),
    ("outer_dma_write", 0x30220, 0x4c, "0fd491e26ea119ad36ccc54950a0deddea7ec8f4a81ddddf2b6d80066df3a049"),
    ("outer_flush", 0x32f34, 0x18, "e61a69be34b214be9b87fa2d40c3139e6de6b5415c22048318635fd902678368"),
    ("outer_enable_interface", 0x5d4f4, 0xa8, "865f922491d3cb36b47f77470bd940910cebdf340f11fbaf9aff64acbfb7d909"),
    ("outer_deliver_response", 0x36c68, 0x28, "73d62d38541d50dbd9e4b18b5be27783278f78cda4c5085c84f36a4c83006257"),
    ("init_symbol", 0x69e50, 0x10, "000d055c788abaf42816445adfc8ea936cc31bb2a9e5d56f99a1020f9af217a1"),
    ("command_flush_symbols", 0x6be10, 0x30, "c71c5e0e53cff430a68564402f17aaaf5bce3f05baf9f2c42666b05f528c7223"),
    ("loop_dma_symbols", 0x6c260, 0x180, "ef73bb9663e05aca96d1174b30756a6387cff81aa94cbe9dfd43e16efd4477ad"),
    ("platform_symbols", 0x6cd70, 0x20, "78306ff4da9fac4586be88c8e1b9f045a4b5db5166cf2c824f97cb26ca5b0521"),
    ("init_name", 0x67b07, 0xe, "a6372ef9578cb48cc8c1604a874e140b7535ba87e84f26669c0966f1f44ed6c3"),
    ("command_flush_names", 0x688a1, 0x25, "dea3022713a845eaad21201d30d13cada71ac39f0f49adb61af4f656a25692a4"),
    ("loop_dma_names", 0x68dd9, 0x154, "a2e62d6f169d6379753c6ef6ace58bef0bce0afe13cc29a213c5bd2219336053"),
    ("platform_names", 0x698a5, 0x32, "c00f46d2ec89f6db17d54164c4dbc8aa615a3f4471b69445782608543a69f97d"),
    ("outer_section_names", 0x79098, 0x4a7, "f54ad2922e3d4a39bdb26a36a249301a78dae5e6b4e190d1d84ea03427f64a86"),
)
# Fixed operands only. Complete bodies above include the intervening code;
# neither this list nor the projection is an instruction-set emulator.
_FRESH_INIT_ARM_SITES = (
    (0x5ce8, 0xe51f7bc8), (0x5cec, 0xe3a08000), (0x5d04, 0xe5848008),
    (0x5d10, 0xe3a00000), (0x5d64, 0xe3e00000), (0x5d68, 0xe5840008), (0x5d74, 0xe1a00006),
    (0x5d50, 0xe1a06000), (0x5d54, 0xe3560000), (0x5d58, 0x0a00006a), (0x7e4, 0xe1a05000),
    (0x7e8, 0xe3550000), (0x7ec, 0x0a000008), (0x860, 0xe1a03005), (0x868, 0xeb008952),
    (0x22dc8, 0xe1a04003), (0x22dec, 0xe1a00004), (0xec58, 0xe1a05000), (0xec5c, 0xe3550000),
    (0xec60, 0x0a000003), (0xec6c, 0xe1a00005), (0x266a8, 0xe1a04000), (0x266ac, 0xe3540000),
    (0x266b0, 0x0a000001), (0x266b4, 0xe1a00004), (0x263c8, 0xeb00037e), (0x263cc, 0xe1a05000),
    (0x263d0, 0xe3550000), (0x263d4, 0x0a000003), (0x263e0, 0xe1a00005), (0x27254, 0xebffff80),
    (0x27258, 0xe1a0a000), (0x27288, 0xe35a0000), (0x2728c, 0x0a000048), (0x27298, 0xe1a0000a),
    (0x273f4, 0xe1a0000a), (0x27080, 0xe5d4008c), (0x27084, 0xe3500000), (0x27088, 0x0a000002),
    (0x27090, 0xe59f020c), (0x270a4, 0xe5940088), (0x270a8, 0xebffe578), (0x270ac, 0xe3a020fc),
    (0x270b4, 0xe5940094), (0x270bc, 0xe5941118), (0x270c0, 0xe59421cc), (0x270c8, 0xebfff7d5),
    (0x270e0, 0xebffe52c), (0x270e4, 0xe1a09000), (0x270e8, 0xe3590005), (0x270ec, 0x1a000003),
    (0x27108, 0xe5941114), (0x27110, 0xebfff7be), (0x27118, 0xe35a0000), (0x2711c, 0x0a00001f),
    (0x2712c, 0xebffe575), (0x27130, 0xe5960000), (0x27134, 0xe5951000),
    (0x27138, 0xe1500001), (0x2713c, 0x0a000005), (0x27160, 0xe5970004), (0x27164, 0xe3500000),
    (0x27168, 0x0a000010), (0x27150, 0xe3a00002), (0x27198, 0xe3a00002), (0x271a8, 0xe3a00009),
    (0x271c0, 0xe1a00009), (0x271b4, 0xe5c4008c), (0x27210, 0xe59f0134), (0x27218, 0xe5869004),
    (0x27234, 0xe59400a8), (0x27238, 0xe5860014), (0x2723c, 0xe3043e20), (0x205e4, 0xe3a06005),
    (0x205ec, 0xe5d70000), (0x20618, 0xe3a02001), (0x2061c, 0xe5c12000), (0x20694, 0xe3a02000),
    (0x20698, 0xe5c12000), (0x2c184, 0xe5950020), (0x2c188, 0xebffd121), (0xeb88, 0xeb004667),
    (0x28204, 0xe3a03009), (0x28208, 0xe2842068), (0x28214, 0xebff7b21), (0xf0, 0xeb001b7e),
    (0x18, 0xe59ff014), (0x26df8, 0xe3a020ec), (0x26dfc, 0xe59f1074), (0x26e00, 0xe28400a0),
    (0x26e04, 0xeb001606), (0x7580, 0xe3a00201),
    (0x27098, 0xe3a00001), (0x2709c, 0xe5c4008c), (0x270f4, 0xe5c4008c),
    (0x27144, 0xe5c4008c), (0x27170, 0xe5c4008c), (0x205a4, 0xe3a06000),
    (0x20608, 0xe5c70000),
    (0x263bc, 0xe5d42030), (0x271d8, 0xe1a09002), (0x2820c, 0xe59f1480),
    (0x28210, 0xe1a00003), (0x6eb4, 0xe59f426c), (0x6eb8, 0xe0800080),
    (0x6ebc, 0xe7841100), (0x6ec0, 0xe0840100), (0x6ec4, 0xe5802004),
    (0x6ec8, 0xe5803008), (0x6f20, 0xe59f9200), (0x6f94, 0xe0848084),
    (0x6f98, 0xe0896108), (0x6f9c, 0xe5961008), (0x6fa0, 0xe3510000),
    (0x6fa4, 0x0a000007), (0x6fa8, 0xe7992108), (0x6fac, 0xe5960004),
    (0x6fb0, 0xe12fff32), (0x2c170, 0xe1a04000), (0x2c178, 0xe1a05004),
)
_FRESH_INIT_ARC_SITES = (
    (16, 0x26668, 0x621f7c00), (16, 0x266c4, 0x08084088),
    (16, 0x266c8, 0x67e07a01), (16, 0x25824, 0x61ff7c00),
    (16, 0x2586c, 0x50207c00), (16, 0x2588c, 0x20000400),
    (16, 0x259c8, 0x40277f00), (16, 0x259dc, 0x601f7c00),
    (16, 0x259e8, 0x14001a84),
    (16, 0x246d0, 0x50410400), (16, 0x246e0, 0x10070404),
    (16, 0x3a984, 0x61df7c00), (16, 0x3a9b8, 0x10071f00),
    (4, 0xbdc4, 0x081f0000), (4, 0xbdcc, 0x60007c00),
    (4, 0xbdd4, 0x68207c00), (4, 0xbddc, 0x601f7c00),
    (4, 0xbde4, 0x14008000),
    (16, 0x266cc, 0x20000182), (16, 0x25874, 0x57e0fa08),
    (16, 0x25878, 0x2000278d), (16, 0x2587c, 0x081fa000),
    (16, 0x25880, 0x40007e03), (16, 0x25884, 0x40000200),
    (16, 0x25888, 0x38000000), (16, 0x3a980, 0x61e08200),
    (16, 0x3a9ac, 0x67e79f00), (16, 0x3a9b0, 0x20000141),
    (16, 0x3a9b4, 0x09e70100),
)
# Source function, source VA, target function, optional retained RELA position.
_FRESH_INIT_ARC_CALLS = (
    ("CmdInitialize", 0x2461c, "Core_LocalClear", 0x72930),
    ("CmdInitialize", 0x246ac, "Platform_EnableInterface", 0x7293c),
    ("Core_Command", 0x25840, "Dma_Read", 0x72f0c),
    ("Core_Command", 0x25848, "Dma_Sync", 0x72f18),
    ("Core_Command", 0x258b0, "CmdInitialize", None),
    ("Core_Command", 0x259cc, "Dma_Write", 0x72f90),
    ("Core_Command", 0x259d4, "Dma_Sync", 0x72f9c),
    ("Core_Command", 0x259d8, "Arc_FlushWrites", 0x72fa8),
    ("Core_Command", 0x259ec, "Platform_DeliverResponse", 0x72fb4),
    ("Core_Loop", 0x266e0, "Core_Command", 0x733a4),
)
# Complete selected original outer bodies, not an image-wide address delta.
# ELF virtual addresses, bundled file offsets and device addresses are distinct.
_PPB_BANK_BODIES = (
    ("Core_Run", 2, 0x4dc4, 0x51cc, 0x2fc68, "26e570670d39d004b02634814b88d8b510d6294eb268f08599d25c02e22403f3"),
    ("Core_CircBuffer_Get", 2, 0x4d10, 0x4dc4, 0x2fbb4, "4e0746e00040cedaee3d0275eaa7c1a14fa9f4dc056a60511dfd9b423a901a90"),
    ("SystemCore_MonitorIL", 2, 0x41e8, 0x428c, 0x2f08c, "201e15dcc371f4367107ca43b8c71e16ccc281b351cfbf2db4dcbdbbf8276685"),
    ("VideoParameters", 4, 0x80dc, 0x8218, 0x32f80, "e4525894db6bffca8c049ff0607d87d17f98cfea965b3f2af785de31b7b82a74"),
    ("Core_OrderPIF_Release", 4, 0x903c, 0x907c, 0x33ee0, "71bd2df89b0a19472ee16e27751c344b9c83a552d789eacf6822d39e533a5c14"),
    ("Core_DeallocatePPB", 4, 0x907c, 0x91c4, 0x33f20, "2117ff7294daa5e9da4d290f92d28b28ae3a04b927491427e7f7885c1e90b1bf"),
    ("Core_OrderPIF_ReleaseOnLatest", 4, 0x91c4, 0x92a8, 0x34068, "8fd0c47de533593c0c4eacb9b2c92bf05cc32a64de05da36ede4eeb1c87a06a7"),
    ("ChannelCore_MonitorIL", 4, 0x9850, 0x9ad8, 0x346f4, "697350ac240ad589c6576efbdb3196d6f8c93f55799720e6113215232bb9e047"),
    ("Core_CopyDramToLsram", 4, 0x9e74, 0x9f90, 0x34d18, "a78e06a80f25a603981e888fa35ccf9e811dca10acdc4d71b919bd8902f88a1e"),
    ("System_Activate", 4, 0x9f90, 0xa0b4, 0x34e34, "9530baaafd18492433c0f9fdca77cfcd8d0ad1aa982e221303d6cd54de17521e"),
    ("Core_AttemptDecode", 4, 0xa258, 0xa844, 0x350fc, "02d9516500364df9de2e00eef06024988f55cac6afe09c3b34fdf6613d1d9cfd"),
    ("AttemptRelease", 4, 0xab44, 0xac18, 0x359e8, "e48508989d3e07060459c9a4fab4d2d3b397d088de3e8477061647a29d353b1e"),
    ("Core_AttemptIL", 4, 0xac18, 0xad90, 0x35abc, "b2809c8ed5d4b625f5bf0e3fc02d79cf36502fd47c3124d9958766a2de5c2c6e"),
    ("AllocatePPB", 4, 0xad90, 0xb080, 0x35c34, "749dd3f410d7ccabe322d3be1f972f68edea06623bac913614b3535024eac134"),
    ("PPB_Video_Address", 4, 0xb080, 0xb0fc, 0x35f24, "fd889611f1240599a758bb10fe64b35cd82f723f9c65027736b4ca5766280c26"),
    ("Core_AttemptPPBAssignment", 4, 0xb0fc, 0xb38c, 0x35fa0, "c7e91dc4bcd1f99f7eb720d709e5d60c522ca65c49127e2bd41d08f901b79adc"),
    ("Core_CircBuffer_Put", 4, 0xb554, 0xb610, 0x363f8, "b2b2c7a2b5d695d0e732f54fdc87cd1ced68f62814469793999d46cd518e9375"),
    ("Core_AttemptDisplay", 4, 0xb610, 0xba98, 0x364b4, "7451b2344541e6428f8cf4e7da1df523acea2739bc40c4c881561e6441ca29c4"),
    ("Core_PPB_From_Address", 4, 0xba98, 0xbac8, 0x3693c, "edfdd3c4305efa139464b9e1b9fa67614ee6ff684e96de4156f63564261609b0"),
    ("_udivmod", 4, 0xbc04, 0xbd18, 0x36aa8, "baa89531a255d78c4d4f65b6ca83520e9fd15d57622d3f801e31362f67a3023f"),
    ("Platform_VideoStripeHeight", 4, 0xbd90, 0xbdc4, 0x36c34, "d1c6cb2f1e6c67710173229a20ab155c521377eac7de02ce860de44b7e9eab17"),
    ("Platform_DeliverPicture", 4, 0xbdec, 0xbe38, 0x36c90, "3e32bb1c023229ce130c2b8184586eb92dddf88eefca2b54948347e5b7b1b21a"),
    ("Platform_UpdateReleaseQueue", 4, 0xbe38, 0xbe70, 0x36cdc, "009ee74b4c5cb5684519e88dcda070e71a1df472a97ab19976761a57d184b5a4"),
    ("CmdInitialize", 16, 0x245ec, 0x24788, 0x47180, "f6a261cb43d156ce01a7a04419811312e5750c4271ff1f99f6c4f6f17cb34c72"),
    ("CmdChannelOpen", 16, 0x24788, 0x24bb4, 0x4731c, "299ddffe5d7c502e0d0df9be965842c76f7de59cd09b30c65ed00fb807a49e22"),
    ("CmdChannelStart", 16, 0x24dac, 0x254d4, 0x47940, "a13392f375549b5945eb02558207542e70632ebd3d8a9d35b2a19050c996bb48"),
    ("Core_Command", 16, 0x25808, 0x25a0c, 0x4839c, "6bce8ec0e12cbc0b5e9193c6a4d88d84e3ab156ae47df80743073ac2529c508b"),
    ("Core_SetPIF_NoDisplay", 16, 0x25f10, 0x26080, 0x48aa4, "cf24692882521152ddd02efe85eef379964a8b42cd88def02294d7817a707699"),
    ("Core_ReleasePPB", 16, 0x260e4, 0x26144, 0x48c78, "44e00b85365feb9e2a1d82da5a8dab81807830a46133df7c72987db0a07fb43a"),
    ("Core_GetUndeliveredPPBs", 16, 0x26144, 0x26270, 0x48cd8, "7bd43689268dc47f4cddc6da57a25bad8e57db03f48fda4b67f5649cf4473797"),
    ("Core_Late_PPB_Release", 16, 0x26330, 0x263a8, 0x48ec4, "58114adacd64a3500e950dff3c8d1529aa38610614f0605a98bd0f0451715cf9"),
    ("PopulateEmptyPPB", 16, 0x263a8, 0x26444, 0x48f3c, "0ac2aa670ad6a1c001190de6b1618dfbc40843c0669a35f40e175b564766be35"),
    ("Core_ChanInitialize", 16, 0x266f8, 0x26974, 0x4928c, "11aeb52ef9f2a3c8ab99cf099c75eade7a69abe991e3561a05f423e54b1b4d71"),
    ("Core_StartChannel", 16, 0x26974, 0x26ba0, 0x49508, "8fe220a7487341e1975615f11a7553681f25ef618556f6f03838a87327d8be4b"),
    ("Core_StopChannel", 16, 0x26ba0, 0x26e10, 0x49734, "b89f444e9f18bf1a998993eb4ea5f81fc733b6ab18343b5fd1e1d942b589a4b7"),
)
_PPB_BANK_METADATA = (
    ("elf_header", 0x2ea60, 52, "23799a2426f4a4d638beb46ecaf49047463bf78b1527bcb1880f6473ea02c380"),
    ("section_headers", 0x79540, 2200, "5ba0754ef57c2d51ba8362ac7c9778b6d3f77a72466f9a09588337a8d99ea7d6"),
    ("section_names", 0x79098, 1191, "f54ad2922e3d4a39bdb26a36a249301a78dae5e6b4e190d1d84ea03427f64a86"),
    ("symbol_table", 0x69b70, 13392, "d148d757c8d16a65a1ed1f52957317867986847e5e626a551eaafcaf6fcc1ccb"),
    ("symbol_names", 0x67a95, 8410, "4a9e326f9ad7c3be6015519586e7decab7a5647be27b7bc00578f1ef2f281b4a"),
    ("slice_relocations", 0x6d020, 1008, "31d0234e0f31d3ed02cee74f82a66c64f897a69575888b83336f9fcabfe631a7"),
    ("picture_relocations", 0x6da34, 3132, "5c8ac2c91e08eeabe4567c2d908d2a7841c700b6a77c01b9329e727bf7eaf091"),
    ("text_relocations", 0x72780, 26052, "8c1c3eb2f61ad26f9278028b0aa9a596650345df13b1dd0d6d806cf677b9bba9"),
    ("vendor_extension_declarations", 0x67a25, 112, "50144f0baa420310c9ac35d5739a4e90af2f13e07bb65daa87894472f695ed00"),
)
_PPB_BANK_REGIONS = _PPB_BANK_METADATA + tuple(
    (name, offset, end - start, digest)
    for name, _, start, end, offset, digest in _PPB_BANK_BODIES)
# Ordinary H264 producer bodies, qualified by their original outer ELF section.
_PPB_SOURCE_BODIES = (
    ("PopulatePPB", 5, 0xf2b0, 0xf760, 0x3a154, "4d8a71519ab56a688c61948d5aec9bc15da67ee3525df3d638fc478895dceff6"),
    ("EndOfPicture", 5, 0xf760, 0xfa5c, 0x3a604, "c03df07328dffba01e10fe02a5a705c8be04c07bd81992d37852628f2a69a17b"),
    ("H264_DecodePicture", 5, 0xfda4, 0x10258, 0x3ac48, "c58523f87b6082fdfe61674df64a591baad1f20ee0025c826a9fbcc3d8a6d598"),
    ("H264_StartOfPicture", 5, 0xd8bc, 0xdb60, 0x38760, "20a9f69edef44edfcc5506a8f061ec6f5150944dbb5cce2822b03fc5e45da5b8"),
    ("H264_IsCFP2", 5, 0xdb60, 0xdc1c, 0x38a04, "a42b7f1e8892b5707149d84aeed42d5115d6ebcffdb18480a4c10674c0af6c79"),
    ("H264_ParseSPS", 16, 0x2aee4, 0x2b658, 0x4da78, "7604f8afb6bd6c75ae532d5599b0068f48d94a8574405d5347c92e687be169c4"),
    ("H264_ActivateSPS", 16, 0x2b658, 0x2b724, 0x4e1ec, "7d875e642ba9f2ee2230f7298f48ae8ebc3cdaa1983035bfb6e50773da43c33a"),
    ("ParseSlice", 3, 0x7590, 0x7d68, 0x32434, "dee4395033ddbf46b0fdc32a68d502ef020efc95ac7fbca45c84ec4175478208"),
    ("Core_PopulatePPB", 4, 0x9344, 0x9548, 0x341e8, "2b3cde5b8b4eef00eedd2c7fa4829324f428f7cb89d01eee685db5a5f6de6805"),
)
_PPB_SOURCE_REGIONS = _PPB_BANK_REGIONS + tuple(
    (name, offset, end - start, digest)
    for name, _, start, end, offset, digest in _PPB_SOURCE_BODIES) + (
    ("h264_slice_relocations", 0x6d410, 1572, "bd97fd812ccaf604c9c028d73fc8ddfdb2e36ff64ea4694fe6a61c1444e6c00e"),
    ("h264_picture_relocations", 0x6e670, 4596, "dc14dd531617af502be4f2e96dc41f6d88dff82f386ebd506ca3805ebe8a33fb"),
)
MAX_PPB_SOURCE_REGIONS = 64
MAX_PPB_SOURCE_BYTES = 96 * 1024
MAX_PPB_SOURCE_RELOCATIONS = 2685
# Saved DRAM context provenance, not an alias for the active ARC-local core.
_PPB_SAVED_ARM_REGIONS = (
    ("root_getter", 0x898, 0x8, "fed8d9727528a2abbf8b8b5c258b5d558e3192d5fc3232bc4a7f0a1c91cd6e38"),
    ("open_wrapper", 0x8a0, 0x2f8, "852df88584df06c581c4e32fec35d020233fd042a9c5a4579cdfbce26a535f12"),
    ("sm_open", 0xa2a4, 0x110, "152b7de5dac90649b746ae70ed0c9a8709c324785cbca4e56ef040cd02982338"),
    ("controller_create", 0xe8b0, 0x44c, "5a39a3cd8e72b709a02706b0d432eabb56aa7488d90929fd5330ab6d0bd14c45"),
    ("channel_open", 0xf7e4, 0x408, "723911a2a94585093da0b4caebd2ba2a20b2c8bc87c4e91dd42646ae95a3a092"),
    ("malloc_wrapper", 0x20510, 0x1c, "f5b42fe2871ad003de5020485b5ea214874abbdb87e2dbf0ceb22a1c590b9489"),
    ("malloc_manager", 0x7194, 0xc4, "38ba97a660f9b366122dcd5026e19c0611a5ca1b5ebd23f0bb7a99d78cb86675"),
    ("small_heap_init", 0x2578, 0xa0, "708f312582aa7e95b2aeba557528aad7791470d9e0219451d0b22c15f814e246"),
    ("variable_malloc", 0x2d30, 0x44, "eefb0ec02f6d9889205afdade868a50321ce22ea9af931645ff8a7c2db08d989"),
    ("context_config", 0x252a4, 0x6b0, "7619bc9a6eff1b1ba1c3ea8e8f719eb8ce064f3cd15ff65a584f5c7e165fc357"),
    ("context_allocate", 0x25c18, 0x540, "b413d045432e9927c877cbc76c507d9d2868cd41632c54f92ca782d35236d376"),
    ("arc_open_builder", 0x27480, 0x144, "c356d9c46eee35ea6dafb60e7e1ef49cf37c77726e4ba14e5e192f9cc2c50f91"),
    ("virtual_to_physical", 0x1fe6c, 0x9c, "bb0e8f8634a6ae4fb7029ec72f0a1e8c524e7e25f7285105430c4987473f75e2"),
    ("manager_publish", 0x250bc, 0x144, "fa480a332eccac24ca614e9e6f710a64a8cb917421549715f83ed06936d10bf4"),
    ("owned_video_regions", 0x28310, 0x53c, "f0c0e9c39268d575a87c292eaeabc3704c015535c93c1456e256910ca53c1134"),
    ("manager_create", 0x2b3f0, 0x1b4, "3e3c9c3b797ebeb13fc6c1ef116a617973f4f50e41730e82bf350b0f8f125b8e"),
    ("manager_allocate", 0x2b628, 0x1a0, "ce3360c94599082eace9edf99b103215c332a13ae17b9980cdb542a3cbedb2d6"),
    ("map_create", 0x200d8, 0x13c, "681b71b44ebdca366432f0a0df8b8e685321deb4991ac2c83096d925ca7848a3"),
    ("main_map_create", 0x294ec, 0x244, "f7a2df49ccbb0ba7663fca21285d437c3ca231a5f351ce063d41bb2d6d9ffe70"),
    ("child_map_create", 0x2906c, 0x378, "affcb72dd72784f9c6329eba4fee513e9c70a6b861c2780de5825aaf4dee6868"),
)
_PPB_SAVED_ARC_BODIES = tuple(body for body in _PPB_BANK_BODIES if body[0] in (
    "CmdChannelOpen", "Core_ChanInitialize", "System_Activate", "Core_CopyDramToLsram")) + (
    ("Core_CopyLsramToDram", 4, 0x9c3c, 0x9d48, 0x34ae0, "32f163266b8f614b9e46b94bc4a9b20863606b0cbad567b3b6a496f5d7866c64"),
    ("System_Deactivate", 4, 0x9d48, 0x9e74, 0x34bec, "f185b940f905ee5c4e28f097434d8757b3e5e9a5ccba6fe47422caf25e892e20"),
    ("System_ChannelSwap", 4, 0xa0b4, 0xa13c, 0x34f58, "8315bb7a6c172ee672fcc71f07e09dadca1d5b5135d752f967d175f1c17f4942"),
    ("H264_Deactivate", 5, 0x10258, 0x10270, 0x3b0fc, "053a80d9cb4f28d91a3f4bcea39772d24496d9adeba423be64c87e083454845e"),
    ("H264_Activate", 5, 0x10270, 0x10408, 0x3b114, "fb737971e94e0175cb4745c697466240bce61ca705ad13d03e100bac9a9b832f"),
    ("Platform_DrvContextSize", 16, 0x3b304, 0x3b30c, 0x5de98, "837494e6a5e29cf8ca42cf997921cc319edae871aebc286e75b44e30c933dd7b"),
)
_PPB_SAVED_REGIONS = _PPB_BANK_METADATA[:5] + _PPB_SAVED_ARM_REGIONS + tuple(
    (name, offset, end - start, digest) for name, _, start, end, offset, digest in _PPB_SAVED_ARC_BODIES) + (
    ("arm_init_context", 0x54c, 0x34c, "807fac6ec7a10ccde3cda20dd535ff9462b00f82ccb4cdc71c575b6cf94ac109"),
    ("video_heap_parameters", 0x7534, 0x70, "1ef4bebb542793521b81d76f3625db33c657dcde88d5a77cb070de87b62877ff"),
    ("root_literal", 0x6fc, 4, "5d41a43f0a6983f407fc72552a64a4ea2e7bd6565d4df18496b2c7c102017755"),
    ("malloc_literals", 0x72a8, 12, "44f11a135d90efd7906904d4f248c0219fa3901d81fcb4b5b5247079263d40f5"),
    ("video_heap_literals", 0x76b4, 8, "03aa7bce575c23277ee32aabcdb77e2d8a42ccebb63fc4e5ec6305d9fdd69ec8"),
    ("h264_context_table", 0x2e5c4, 560, "6babb2cd1d41212ccac94e58ee075d920f698690849df269ed4a5da122f913bb"),
    ("flat_mpu_setup", 0x2ca94, 0xb4, "58896ddaa10dad5a760055218f23d398f4b28c0a7dc6a0b9f695b25dc862da52"),
)
MAX_PPB_SAVED_REGIONS = 64
MAX_PPB_SAVED_BYTES = 64 * 1024
_PPB_CONTEXT_OBJECT_BYTES = {"H": 0xa84c, "C": 0x378, "Q": 0x1f4, "M": 0x64}
_PPB_SAVED_SCALARS = (("core_word", 0, 4), ("metadata_extra_word", 0x80, 4),
    ("producer_pool_value", 0x21c, 4), ("metadata_pool_value", 0x33c, 4),
    ("ppb_flags", 0x354, 68), ("bank_descriptors", 0x3fc, 144),
    ("bank_bytes_and_count_word", 0x48c, 8), ("reader_pool_value", 0x530, 4))
# Separate STOP proof: the saved-context option's receipts/output stay frozen.
_PPB_STOP_ARM_REGIONS = (
    ("host_stop_handler", 0x4288, 0x3a8, "0fa6a9603dd61c135d027f33708dbcada8df969107fe9ef5f9f7c728d205851a"),
    ("slot_stop_wrapper", 0x125c, 0x88, "7412ad146fa32d0f03d2a11f08758cd8c29b27a0de198dffb43cfa48a9394bb6"),
    ("decoder_stop", 0xef10, 0xac, "dac38f265061167d8a22a7b3e49e128209206062e6138f41fed094c2d0a9b601"),
    ("arc_stop_builder", 0x27750, 0x8c, "f95765626233c26ccf54e514bbd6c17216122da1bf71820dc6ecd45e386c9ff9"),
    ("arc_transport", 0x2705c, 0x16c, "bd461670f479a8e1f005d75357912eee0b0b61d875c6c87c7a10f79d9303d6f8"),
    ("decoder_close", 0xf6e4, 0x100, "be21c591f9cbeeca67dd5479d1388eed2edf80be4977f0ce9353a535311d79b1"),
    ("stop_stats", 0x24edc, 0x110, "5e3a2ec79931b1e7d4d992654a6816139d6a233188475c6a52e516166e426651"),
    ("stop_noop", 0xe548, 4, "379bec29dccd0a93c94826144d7ef6e42fab64ef195a3b8313a16926f66f388f"),
    ("stop_command_literal", 0x27ab0, 4, "38c07ee2c1401fe213b333a1fbb4ba7d716c9d5df5df4077fbb53a3daa977748"),
)
_PPB_STOP_ARC_BODIES = tuple(body for body in _PPB_BANK_BODIES if body[0] in ("Core_Command", "Core_StopChannel"))
# Record file offset, source section/site, numeric symbol and qualified target, delay, independent fuse.
_PPB_STOP_RELOCATIONS = (
    (0x6df2c, 4, 0x9cb0, 645, "Dma_Write", 2, 0x537c, True, "b39012686f2223e97d55705f4bb04c90aedf7bde8a3517d7e429ec51d53f9f76"),
    (0x6df50, 4, 0x9d14, 645, "Dma_Write", 2, 0x537c, True, "f8eed0bf46a2a18366e47560ee74ade1406689f55cb8643698d7c0001aa08f26"),
    (0x6df5c, 4, 0x9d1c, 644, "Dma_Sync", 2, 0x5364, False, "e561ecfa8bbc419599bab9149fae953092f5d0aef821a58263c5a865830da0f7"),
    (0x72f48, 16, 0x25910, 626, "Core_StopChannel", 16, 0x26ba0, True, "bb77d155fa95c33c19658c64f20878e0b02df728e6d73eb05009015eb9f8146d"),
    (0x72f90, 16, 0x259cc, 645, "Dma_Write", 2, 0x537c, True, "092699e130227a0c95120f05d44cee1d633e6fef9f0f9d00b9bc93b584e2154b"),
    (0x72f9c, 16, 0x259d4, 644, "Dma_Sync", 2, 0x5364, False, "78784bb8f3350392888e392b7aa756a88a58ca1686660f0955a9b7e037695657"),
    (0x72fa8, 16, 0x259d8, 556, "Arc_FlushWrites", 4, 0x8090, False, "1c0ff9fd1768c937382661b97481d5ef95dfe9325e5f71b9fa02dd37d637ec59"),
    (0x72fb4, 16, 0x259ec, 801, "Platform_DeliverResponse", 4, 0xbdc4, False, "201fb50630e864fa1a685323781ec31d823593e1e4886d1eed937b16cd5c7b33"),
    (0x73518, 16, 0x26db4, 600, "System_Deactivate", 4, 0x9d48, True, "a569d6cf02f8665150c1f1d56903de2bc4e9d24c17b5c50eb30d03bd2da891a7"),
)
_PPB_STOP_REGIONS = _PPB_SAVED_REGIONS + _PPB_STOP_ARM_REGIONS + tuple(
    (name, offset, end - start, digest) for name, _, start, end, offset, digest in _PPB_STOP_ARC_BODIES) + tuple(
    (f"stop_relocation_{record:x}", record, 12, digest) for record, *_, digest in _PPB_STOP_RELOCATIONS)
MAX_PPB_STOP_REGIONS = 64
MAX_PPB_STOP_BYTES = 64 * 1024
MAX_PPB_STOP_RELOCATIONS = 9
MAX_PPB_STOP_RESPONSE_BYTES = 252
# Separate fixed, per-picture DRAM publications; never follow a saved pool word.
_PPB_FIXED_METADATA_BODIES = tuple(body for body in _PPB_BANK_BODIES if body[0] in (
    "Core_Run", "Core_CircBuffer_Get", "VideoParameters", "Core_DeallocatePPB", "Core_AttemptDecode",
    "Core_CircBuffer_Put", "Core_AttemptDisplay", "Core_PPB_From_Address", "Platform_DeliverPicture",
    "Core_ChanInitialize")) + tuple(body for body in _PPB_SAVED_ARC_BODIES if body[0] == "Platform_DrvContextSize") + tuple(
    body for body in _PPB_SOURCE_BODIES if body[0] == "Core_PopulatePPB")
_PPB_FIXED_METADATA_REGIONS = tuple(dict.fromkeys(_PPB_SAVED_REGIONS + tuple(
    (name, offset, end - start, digest) for name, _, start, end, offset, digest in _PPB_FIXED_METADATA_BODIES) +
    tuple(region for region in _PPB_BANK_METADATA if region[0] in (
        "slice_relocations", "picture_relocations", "text_relocations")) + _ARM_PPB_HANDOFF_REGIONS))
_PPB_FIXED_METADATA_CALLS = (
    (0x6e4d8, 4, 0xb6b8, 646, "Dma_Read", 2, 0x53c8, True),
    (0x6e4e4, 4, 0xb6c0, 644, "Dma_Sync", 2, 0x5364, True),
    (0x6e520, 4, 0xb8fc, 645, "Dma_Write", 2, 0x537c, True),
    (0x6e52c, 4, 0xb904, 644, "Dma_Sync", 2, 0x5364, False),
    (0x6e538, 4, 0xb938, 645, "Dma_Write", 2, 0x537c, True),
    (0x6e544, 4, 0xb940, 644, "Dma_Sync", 2, 0x5364, False),
    (0x6e550, 4, 0xb988, 802, "Platform_DeliverPicture", 4, 0xbdec, False),
    (0x6d2b4, 2, 0x517c, 612, "Core_PPB_From_Address", 4, 0xba98, False),
    (0x6d2cc, 2, 0x51a4, 578, "Core_DeallocatePPB", 4, 0x907c, True))
MAX_PPB_FIXED_METADATA_REGIONS = 64
MAX_PPB_FIXED_METADATA_BYTES = 96 * 1024
MAX_PPB_FIXED_METADATA_CALLS = 9
# Fixed stock host contract only. These hashes are independent local fuses;
# the public firmware identity remains unchanged and this helper is test-only.
_STOCK_HOST_COMMAND_REGIONS = (
    ("handlers", 0x3ca8, 8836, "5bccf2815b633adec748196859a327a6957b9841f49f28a72f7bc2e27c97f4be"),
    ("dispatcher", 0x5f2c, 2648, "9af9fb8464ca471a065039afecfc37ea0426a6a2560674fc68cd8e3a08b16ce0"),
    ("stream_ack", 0x6984, 52, "1f9092e749c1a3fb12c5966cdc0439e304027878bcc8b43fc8dadd8a89cf559e"),
    ("clear_arm", 0x206e4, 36, "198a9ab2bd9405ec7cb7e17b1e98f23a564a370215758ad1f2d3500dd6b25292"),
    ("clear_thumb_value", 0x2c688, 16, "ecef68fb180095ac4ba52e4d21271f040e6516c3f613feea4c3067855882561a"),
    ("clear_thumb_fill", 0x2c73c, 142, "40142c13e3b9e4840b17fb10874e1f87ec3fd9dc712b7e678c257e12ac553f8e"),
    ("caller", 0x9048, 496, "fddc1f25c5cda3614b16df8ca7295344b443adee4e41a2cd4146c26d3667719c"),
    ("caller_base", 0x92fc, 4, "fef271f24fb2cf3f477ed2952cf742115f84b7a46d2bca54b3d5b91626bd6984"),
    ("caller_queue", 0x8c28, 4, "7f184bcf7fa75e733ba0b16821477063ad86198931699eba7615d718fb5885d5"),
    ("caller_publication", 0x8fcc, 4, "45c81a95420e3a6faad7914f835787acc2a65ad0094fea16ba55eddc02836600"),
    ("stream_null", 0x6ab8, 20, "d02d0902db2767ce86847cd8edf5e417707dea4957c728d0ff1ab86a75d65463"),
    ("queue_publish", 0x8afc, 236, "5fccc23e2748e4592dc8841c42e963b53470e7644ef85a7139b52e1a0144fff1"),
    ("context_getter", 0x898, 8, "fed8d9727528a2abbf8b8b5c258b5d558e3192d5fc3232bc4a7f0a1c91cd6e38"),
    ("context_getter_literal", 0x6fc, 4, "5d41a43f0a6983f407fc72552a64a4ea2e7bd6565d4df18496b2c7c102017755"),
    ("start_stack_output_prefix", 0x1bf04, 40, "3a16b7e9650678acffc3ddd927eeb2129bb621aceb9614109c1ef467c6dff4db"),
    ("start_stack_output_literal", 0x1bbd4, 4, "e0a38388779be070015cf72408ffbb625cb9f356f4ce0f9a215b7dd3287edc0d"),
)
_DEBUG_MECHANISM_SYMBOLS = (
    "Arc_UartInit", "Arc_UartPoll", "ArcGetc", "ArcPutc", "ArcCommandBuffer",
    "ReadLine", "MatchKeyword", "CmdPeek", "CmdChannelDramLogControl",
    "CmdChannelDramLogCmd", "WritetoDramLogBuffer",
)
_DEBUG_MECHANISM_REGIONS = (
    ("bootstrap_uart_call", 0x2cbf0, 4,
     "69792597d0c7167e77ec8d6899e17d9bb9022b3de7600f8fadf13200cd816080"),
    ("arm_router_setup", 0xac1c, 96,
     "b76a1fecbf1c0a6125cc93096c013f74fe99c5968cc0a7e139795cf85e55b3ab"),
    ("arm_uart_setup", 0xadf0, 84,
     "51caea5ff8b3f1f4385c3133cdee1a4a6d29958f6e0de0a68e15a730c1087d61"),
    ("arm_uart_io", 0xaed0, 208,
     "301368f7891299acca340c89cc262ecbb7d284eb15434092f49dfd107c945d01"),
    ("arm_uart_formatter", 0xafa0, 68,
     "d3593a0a631af7cfa37b6a3e0ae522c3723a78d0e55c3ee091bb2d9954736b5e"),
    ("arm_uart_literals", 0xb034, 16,
     "bee7654d7dea609c50f097f68a13c9738da2610de08a898b59a6aa7b123e5a95"),
    ("debug_setup_message", 0x5b34, 46,
     "5c733c81aac249ef34cf1361163e392ffe4a105c418ab8054b9a9591202e1867"),
    ("arm_log_bridge", 0x203c4, 20,
     "6d05751328b98f93b9248337840bdf8481fdcd5593f9506d3bffa908fcb815eb"),
)
MAX_DEBUG_MECHANISM_REGIONS = 9
MAX_DEBUG_MECHANISM_BYTES = 1024
_RX_DESCRIPTOR_ADMISSION_REGIONS = (
    ("body", 0x77e0, 184,
     "7cd183a9c2450240eb1595af092f546cad349438c8b30adc05a34e9cec768105"),
    ("caller", 0x8818, 12,
     "99e4349b34fecb1edd15dbeada9d680bae714f57c2861c3669d3faa3dbc0e128"),
    ("mmio_literal", 0x79b0, 4,
     "43e026367524e8c8b0a3342b522c0c77ad5b1966dc917ece2a3120540861febe"),
)
MAX_RX_DESCRIPTOR_ADMISSION_REGIONS = 3
MAX_RX_DESCRIPTOR_ADMISSION_BYTES = 256
_CHANNEL_FIELD_REGIONS = (
    ("init_context", 0x54c, 0x354,
     "766ebdef10b26af75d180b82e886deaa7a35746c8b13748c4705fea13e803b4f"),
    ("channel_api_lifecycle", 0x8a0, 0xf00,
     "531ad25633877503cff370620cdb0f59bd12c52e8435fa224b40c5185a9080d1"),
    ("host_start_root_literal", 0x3de8, 0x4,
     "90235ef9116585e3b06f150c19cc273c8b4098db381d46cf3454ab59501e3688"),
    ("host_stop_start", 0x4288, 0x7d8,
     "edf522cfd3afba69e0acc27824886bfcbe46cccd4f1f33773f726c9985412c3f"),
    ("helper_15d8_direct_caller", 0x4a60, 0xe4,
     "631e5d0ba861cd4d2c1f4ad78d00ed8a340684154456a66684607273fca83827"),
    ("helper_12e4_direct_caller", 0x4c7c, 0x1f0,
     "c3e568b63604cc40b146f14675d764c321a0dc287792d76c7049bac51cc69b48"),
    ("host_close_open", 0x4f88, 0xa80,
     "e9ecbcee4ea0fc4b5a91aa9b8ec6b72c6238c04036242dd0614d59370e290cc1"),
    ("device_start_handoff", 0x5ce8, 0x68,
     "000341a8499a9a1aa9d11f09a163b91782d0aacd2d7446688017bbd5f0bdad1b"),
    ("device_start_output_literal", 0x5ed4, 0x4,
     "b2a40293bd039708aa71125019a5d5a0840248bbd2fb97ed01ca0750eaea3f5a"),
    ("stream_handler", 0x6acc, 0x360,
     "356694900955092be492cd38c3e87852622d3cc4ce6b81f4355542ec35b4c6dc"),
    ("descriptor_delivery", 0x7708, 0x84,
     "a6a638674a3af17ddd321a9fcecf67febf788c1a6a74073081c66bed2bc13fea"),
    ("picture_handler", 0x834c, 0x4ec,
     "e2e7a336ede34b0f3f170cc26476debb81ad4cd0eb3862dd1ab3968db7ee07a3"),
    ("irq19_handler", 0x888c, 0xbc,
     "110a0aa7e8d7845c6ba5854114808d7cc9e067dd2bcac1a746e4a159d15d6ba5"),
    ("reinitialize", 0x89a4, 0x158,
     "4ceb06650fb19599e3f1df548775aba445410ece204a90aea32da520838ac1d6"),
    ("reinitialize_root_literal", 0x8c24, 0x4,
     "5d41a43f0a6983f407fc72552a64a4ea2e7bd6565d4df18496b2c7c102017755"),
    ("xpt_lifecycle", 0x9300, 0xea8,
     "f299c874e032c2e7c7daaa09fcc1fc03eca5a397a56d71aa2481eafd675f09ce"),
    ("decoder_lifecycle", 0xa2a4, 0x428,
     "bc6f8aea8feaaf6c771968f5add329da1b93867f72760a65d5accaa6509fa06e"),
    ("pvr_play_open", 0xbddc, 0x278,
     "d82441ceff63fbf3a90352f54ac2c2f676774e7d64df085cc0b947a181357b97"),
    ("source_record_producer", 0xe110, 0x138,
     "7d82037ed0f6b4d0ab23aace8b9247f7073ff243e6ad8cde6ee53326895e93d2"),
    ("bxvd_channel_open", 0xf7e4, 0x408,
     "723911a2a94585093da0b4caebd2ba2a20b2c8bc87c4e91dd42646ae95a3a092"),
    ("xpt_playback_open", 0x1bc3c, 0x150,
     "5c90f328dde0e471b220a8703fe24a655a4d868707b50f73650f1388562f3e1c"),
    ("clear_copy_wrappers", 0x206e4, 0x48,
     "8fa52c68f1d567cc997032cab51cbed731ea8a8ccf16998883974273a8f56fde"),
    ("memcpy_a32", 0x2c59c, 0xec,
     "1e4214d8199c49b92e61acd6fe6347f3564a79300aa71eb21fce02f269921401"),
    ("clear_thumb_value", 0x2c688, 0x10,
     "ecef68fb180095ac4ba52e4d21271f040e6516c3f613feea4c3067855882561a"),
    ("clear_thumb_fill", 0x2c73c, 0x8e,
     "40142c13e3b9e4840b17fb10874e1f87ec3fd9dc712b7e678c257e12ac553f8e"),
)
MAX_CHANNEL_FIELD_REGIONS = 25
MAX_CHANNEL_FIELD_BYTES = 21 * 1024
MAX_CHANNEL_FIELD_AGGREGATE_REGIONS = 32
MAX_CHANNEL_FIELD_AGGREGATE_BYTES = 24 * 1024
MAX_CHANNEL_FIELD_CALLER_SCAN_REGIONS = 2
MAX_CHANNEL_FIELD_CALLER_SCAN_BYTES = 1024
_MFD_SOURCE_REGIONS = (
    ("source_address", 0x1918,
     "f0402de914d04de20070a0e10140a0e1f8219fe55c10d4e5810081e00031b2e7050092e90c008de510208de508308de5"
     "2830d4e510009de504308de500008de5cc019fe5d820cde1977a00eb0800d4e5020050e33900000a0c009de58060a0e1"
     "2700d4e5010050e33600000a0650a0e11a0e8fe20510a0e18b7a00eb052886e1a8119fe50700a0e1d07300eb542094e5"
     "0700a0e198119fe5cc7300eb582094e50700a0e18c119fe5c87300eb346094e5700094e50120c0e32710d4e5010051e3"
     "2b00000a6c3094e510009de53200a0e1545094e50cc09de5950005e09c0303e0950c05e005c283e00c509de5015045e2"
     "025005e005208ce0062082e0386094e5030051e31d00000aa330a0e1011041e21311a0e1583094e5930000e00c309de5"
     "900300e0000281e0050080e0065080e00800d4e5010050e31300000a160000ea0c609de5c5ffffea2800d4e5010050e3"
     "0500000a0c009de58050a0e1d8008fe20510a0e1507a00ebc3ffffea0c509de5f9ffffea6c0094e50130c0e3d1ffffea"
     "0800a0e314d08de2f080bde80c009de5002082e00c009de5005085e0b0109fe50700a0e1897300eb0520a0e1a4109fe5"
     "0700a0e1857300eb0000a0e3f0ffffea"),
    ("mfd_setup", 0x1bfc,
     "f0412de90060a0e10210a0e10340a0e1000056e30f00000a000051e30d00000a000054e30b00000a8f0f8fe20630a0e1"
     "0420a0e1e37900eb145094e5187094e5000096e5046090e50100d4e5000050e30300000a0a0000eaf041bde88e0f8fe2"
     "d87900ea0810d4e5020051e30400001a2820d4e5000052e30100001a8a0f8fe2d07900eb2820d4e5950f8fe20810d4e5"
     "cc7900eb0410a0e10600a0e1b5ffffeb0520a0e15c129fe50600a0e10e7300eb950f8fe20510a0e1c27900eb260e8fe2"
     "0710a0e1bf7900eb0810d4e50730a0e10020a0e30600a0e1d7feffeb0410a0e10600a0e1fcfeffeb0410a0e10600a0e1"
     "f041bde808ffffea"),
    ("source_record_producer", 0xe110,
     "70402de90060a0e10150a0e10240a0e1000055e32100000a640096e50c0290e5a8119fe50020d1e70430a0e10510a0e1"
     "0600a0e1cdfdffeb0420a0e10510a0e10600a0e1f4fdffeb0430a0e10020a0e30510a0e10600a0e115feffeb0420a0e1"
     "0510a0e10600a0e1d2feffeb0420a0e10510a0e10600a0e1effeffeb0420a0e10510a0e10600a0e195ffffeb040095e5"
     "340084e5080095e5380084e5240000ea0000a0e30000c4e50100a0e30100c4e50000a0e3040084e50800c4e50200a0e3"
     "1c00c4e50000a0e31d00c4e51e00c4e5200084e52400c4e52500c4e52600c4e50200a0e32700c4e50000a0e32800c4e5"
     "0200a0e32900c4e52a00c4e50000a0e3340084e5380084e54000c4e54100c4e5440084e55000c4e55c00c4e50800a0e3"
     "740084e5780084e50000a0e37c0084e5800084e57080bde8"),
    ("selected_picture_call", 0x85ec,
     "14109de520208de2080095e5c41600eb34109de5230e8fe238209de56d5f00ebaeffffea"),
    ("mfd_setup_call", 0x84cc,
     "20308de20910a0e114208de20800a0e1c6e5ffeb"),
    ("source_table_literals", 0x1b28,
     "cccc020070ce0200"),
    ("source_register_literals", 0x1b48,
     "10005400280054002c005400312e204368726f6d6120737472696465203d2030782578001c00540020005400"),
    ("source_table", 0x2cccc,
     "000000004000000006000000010000008000000007000000020000000001000008000000"),
    ("register_write", 0x1e8e8,
     "003090e5012083e71eff2fe1"),
)
# Original ELF bytes only: no runtime relocation, ARC decode or execution here.
_INNER_DESCRIPTOR_HEADERS = (
    (0, 0x2ea60, "7f454c4601010100000000000000000002002d000100000078a6030034000000e0aa040000000000340020001200280037003600"),
    (1, 0x79dd8, "7f454c4601010100000000000000000002002d0001000000689f040034000000584c050000000000340020001300280070006f00"),
)
_INNER_DESCRIPTOR_SECTIONS = (
    (0, 4, 0x795e0,
     "3d00000001000000060000008c7f0000d0430000cc41000000000000000000000400000001000000",
     0x790d5, ".core_critical_code_picture"),
    (0, 16, 0x797c0,
     "a00100000100000006004000743d0200a87e01005476010000000000000000000400000001000000",
     0x79238, ".text"),
    (0, 2, 0x79590,
     "0900000001000000060000000040000044040000d423000000000000000000000040000001000000",
     0x790a1, ".core_critical_code_slice"),
    (1, 3, 0xceaa8,
     "230000000100000006000000e0230000440800007c08000000000000000000000400000001000000",
     0xcdf83, ".core_critical_code"),
    (1, 47, 0xcf188,
     "980400000100000006004000680f040091e90300a892000000000000000000000400000001000000",
     0xce3f8, ".text"),
    (1, 45, 0xcf138,
     "6504000001000000060000006803040091dd0300bc04000000000000000000000400000001000000",
     0xce3c5, ".mpeg_innerloop_code"),
    (0, 54, 0x79db0,
     "9d04000003000000000000000000000038a60400a704000000000000000000000100000001000000",
     0x79535, ".shstrtab"),
    (1, 111, 0xcfb88,
     "c60a000003000000000000000000000088410500d00a000000000000000000000100000001000000",
     0xcea26, ".shstrtab"),
)
_INNER_DESCRIPTOR_WINDOWS = (
    (0, "constructor_context_argument", 16, 0x2672c, 0x492c0,
     "00006062"),
    (0, "constructor_pool_source_initialization", 16, 0x26730, 0x492c4,
     "0082e0610004c161008641620008c262008a0262000ca362007c8042bc050000"
     "007c3f60bc050000203cc72f008e236220742928101a0e10207329280028a041"
     "00002060a038c72f00280a60"),
    (0, "constructor_registration_and_slot_copy", 16, 0x2677c, 0x49310,
     "1c808d08007c5f6070d4ff3f087f01401ffee741057e22800002004000260010107f014000020040002800100"
     "0fc294000060000309b00101ffe077000fc494000040000f000011000866150f80601100c87001000fc0640"
     "e44500000481001000fc0640e44e00009400011000fca641e4ce0000981a01109c060110a0060110ac060110"
     "b0060110b406011010fe9f670086a16080010030000a0140bc06401001fea24010800d0844a30010027e8280"
     "4ca30010002200404881001050a3001054a1001058ab0010007c024000240000007c80680000003000200214"
     "407e0040007c0068000000300006001414800d0818808d08007ae757901c4110648100106c81001070810010"
     "00000240688100108c2401100d0401600d1c87670d030030fc2d00100024cb42000600100406001008060010"
     "107e004022fe9f67000481600001003054078210027e82403081000800fc89411c02000001fe9f6021fe9f67"
     "00000610"),
    (0, "constructor_return_edge", 16, 0x268e0, 0x49474,
     "007c0040e45001003c01011044060010800400303c010108037ea2800088a250"
     "03fea2800088a24002fea280000a004044060010017e8240308100081000ae09"
     "1400ce09007c4040e44e0100248500101800ee091c000e0a007c4040e44f0100"
     "20002e0a24004e0a007c0040306f010028006e0a2c008e0a288500103000ae0a"
     "b48100100480ed0b3400ce0a20800f3838106e0b"),
    (0, "constructor_driver_context_size", 16, 0x3b304, 0x5de98,
     "20800f38ecfe1f40"),
    (0, "activation_snapshot_copy", 4, 0x9fa4, 0x34e48,
     "0000a061057e0080181e0e10141c0e10007ce04168d3ff3f10800708007c5f60bc050000007cdf6170cdff3f"
     "20d4ff2f3c7e2740"),
    (0, "context_dram_to_local_copy", 4, 0x9e74, 0x34d18,
     "043e0e1000360e1000386e63307e8e53101a0e10141c0e10181e0e101c200e1020220e1024240e1028260e10"
     "2c280e10000060620082406200052162009aa65121120020019a066201fe9f62007c1f62801a0530009ac661"
     "20020020009ae6610020c861009ae6610000006280fe1f600081e8570ba2a861a08ef62f0a00a06100a60960"
     "002028602099f62f009a4660009fe7672103002000a66642001c076000242960a078f62f009e476000a44742"
     "017e8a42027aea57097c9f6201000000077e0a80009b2852007c0040001a053082f1ff270083f62f009be667"
     "0102002000200860002429602070f62f009a46601000ae091400ce091800ee091c000e0a20002e0a24004e0a"
     "28006e0a0480ed0b2c008e0a20800f3830106e0b"),
    (0, "context_local_word_copy", 2, 0x52e0, 0x30184,
     "027e4190017e6150fffbe15701800f38000481670002003000004008047e00400084001004fe204000800f38"),
    (0, "allocator_inputs", 4, 0x8758, 0x335fc,
     "007c7f6070d3ff3fac812108a881a1080083e257037e0040a5090020037ec070a4818108"),
    (0, "allocator_publish", 4, 0x886c, 0x33710,
     "008c02400009e057007c3f6070cfff3fa5010020a8810110a0810108a8810110788a00107c80001020800f3800000050"),
    (0, "mpeg_allocation", 16, 0x2da6c, 0x50600,
     "007cff6170cfff3f64800708007cdf6100c2ff3f009aa651607c0710801b0530307e20405c000710641a071070820710"
     "347e0040748007102096b52f80fe1f400001e06702f6ff2778800708007f005068000710009a0660"),
    (0, "record_core_base", 4, 0xa314, 0x351b8,
     "007cdf6170cfff3f"),
    (0, "record_scratch_base", 4, 0xa298, 0x3513c,
     "007c1f63000f0030007c3f63801a0530"),
    (0, "record_prefix_source", 4, 0xa3f0, 0x35294,
     "5c7e874200280a6020d8f52f38fe3f60"),
    (0, "record_prefix_write", 4, 0xa778, 0x3561c,
     "5800e70900280a6000b22c60206bf52f38fe5f6000b20c6003fea88100a2a64103fea68100a2a65102fea681009a2740"
     "207af52f38fe5f60"),
    (0, "outer_attempt_bases", 4, 0xac2c, 0x35ad0,
     "007cbf6170cdff3f21840608181e0e10017ae057141c0e10a20100201c200e10a0240020ffff1f60007c1f6270cfff3f"
     "8605280800a0c0414405c709"),
    (0, "outer_record_read_publish", 4, 0xacbc, 0x35b60,
     "00003f08dcd2ff3f017e00403f7e006086014810c2880708047e0070c2808710037e0780001c0040037e0080001c0050"
     "027e008000800040007cff61801a0530009e2760a0d8f42f38fe5f6000cbf42f1c804708007c3f60000f0030d0c00008"
     "017ae06782feff2718840608007c216800000080007c0040000f0000007c00680000003000020014188406081f844608"
     "047e0040047e00601880461013840608001a2040017e0040149c40101684401013804610"),
    (0, "outer_completion_queue", 2, 0x41e8, 0x2f08c,
     "007c1f6170cdff3f130484080009e26701800f3816040408057e0080007ce04068d3ff3f2486630802fae1570a800f38"
     "19044408007c2140000f000000fc20680000003000c0c008000de36704800f381404a408007c204088d3ff3f007c0040"
     "8ad3ff3f0086204000060040008a4010000c401001fe614015040408248643101400441017040408017e825013084410"
     "16004410047e0140047e006020800f3819004410"),
    (0, "outer_channel_completion", 4, 0x9850, 0x346f4,
     "043e0e1000360e1000386e633c7e8e53101a0e10141c0e10181e0e101c200e1020220e1024240e1028260e102c280e10"
     "007c1f6270cdff3f1f040808382e0e10057e0080007ce04268d3ff3f2486ab09342c0e1000fae657302a0e10ac3d0020"
     "00aaaa52007c3f6270d3ff3f007c404288d3ff3f007c80428ad3ff3f0004c9096c810808007cdf62801a053000046a0a"
     "037ee781009ce74103fee781009ce75102fee781001e0040002c2b602059f72f38fe5f60a04bf72f001cc741"),
    (0, "allocator_boundary_restore", 4, 0x99d8, 0x3487c,
     "20000b08007cff61001b0530ac8108100c000b08009e27608c810810"),
    (0, "mpeg_parser_field", 16, 0x2d49c, 0x50030,
     "141c0e10007cdf6100c0ff3f1c060708007ae057a9030020101a0e1000a803281000ae090480ed0b1400ce0920800f38"
     "18106e0b20d4ad2f0afe1f6078008710a0d2ad2f03fe1f607c004710"),
    (0, "mpeg_final_bases", 16, 0x2e824, 0x513b8,
     "0000e0610082c0620004c161007c5f6200c2ff3f205fad2f6d7e0940d8054908302a0e100005e167007cbf6100c0ff3f"),
    (0, "mpeg_final_core_base", 16, 0x2e8dc, 0x51470,
     "007cbf6270cfff3f05024010"),
    (0, "mpeg_field_copy", 16, 0x2ea18, 0x515ac,
     "6d040908077e0080007cc041001a0530001c076078fe2640a063ad2f10fe5f6078800a08b47e2040001c0760a026ad2f"
     "10fe5f608022ad2f"),
    (1, "inner_loop", 3, 0x2afc, 0x7ad38,
     "043e0e1000360e1000386e63a02f8128107e8e5380548128007cff61000f0000007cbf6114c1ff3f007c7f620010f03f"
     "007c5f6200e1f505007c3f62000f003000fc076a0000003014c00908207ae06701d380284ca408144cc008080040c809"
     "001de76703fcff2721a01f088c8126080083e06781010020908126080002005094810610007c0760ffffff7f008aff2f"
     "00fc2768000000300080001404fee77921a01f0801fe3f608c830610"),
    (1, "inner_prefix_copy", 3, 0x2814, 0x7aa50,
     "0000606221a01f080000c06200244952007c3f62000f003028a40814007cdf6114c1ff3f007f074000a62960a023ff2f"
     "30fe5f60"),
    (1, "inner_mpeg_call", 3, 0x2954, 0x7ab90,
     "e8ff2d402011832800a60960"),
    (1, "inner_mpeg_copy", 47, 0x441e4, 0xbb9e5,
     "043e0e1000360e1000386e63147e8e5300002060007c1f60f0c2ff3fa0eb7b2f80fe5f40204aff2f101a0e10008fff2f"
     "0076f82f"),
    (1, "inner_packet_base", 47, 0x43ea8, 0xbb6a9,
     "007cff6100c4ff3f04812708007cdf6100c2ff3f"),
    (1, "inner_field_transfer", 47, 0x44150, 0xbb951,
     "a4816709a8814709ac812709b0810709c4170710c8150710cc130710d0110710"),
    (1, "inner_consumer_base_and_two", 45, 0x405e4, 0xb7de5,
     "007cff6000c2ff3fc88583080881c308017ae157d98563080000005038814310dc85230801febf60028aa250027ae257"
     "fd8b4d10a2040020f4810d10"),
    (1, "inner_consumer_one_and_three", 45, 0x40640, 0xb7e41,
     "017ae2578203002080fe5f4003fae157f4850d10a2050020b4800310a005002000040160037ae2570201002002fe5f60"
     "b484031003fae157"),
)
# Exact windows of the pinned control path, not a disassembler or a code scan.
_COMMAND_BUFFER_BRIDGE_REGIONS = (
    ("host_init_dispatch", 0x5f44,
     "145084e2456f84e2002100e30010a0e30600a0e1e16900eb000095e5000086e57d0f8fe2001095e5146900eb001095e5"
     "0850a0e3f4219fe5020051e1020041e0a801000a270000ca070042e2000051e1002041e02201000a180000cafd2040e2"
     "020051e1020041e0ac00000a0c0000cac20e62e0010090e04100000a"),
    ("host_init_call", 0x60c8,
     "4900a0e3911300eb0400a0e1fcfeffeb000050e30100000af4008fe2b66800eb1050c4e5f4ffffea"),
    ("fresh_init", 0x5ccc,
     "f0412de9000050e30f00000a145080e2454f80e24d0f8fe2b66900ebc87b1fe50080a0e3000097e5000050e30b00000a"
     "4e0f8fe2af6900eb088084e5040095e5040084e50000a0e3f081bde80010a0e14c019fe5a76900eb0200a0e3f9ffffea"
     "050d8fe2a36900eb68019fe5fd0500eb590f8fe29f6900eb88119fe554019fe5fee9ffeb0060a0e1000056e36a00000a"),
    ("heap_parameters", 0x7534,
     "f0412de90040a0e170719fe570519fe5170e8fe20510a0e19c6300eb005084e5045084e50060a0e3106084e5051047e0"
     "0c1084e5085084e5"),
    ("heap_literals", 0x76b4,
     "00c0ff0304601100"),
    ("init_context", 0x54c,
     "f0402de95cd04de20050a0e10170a0e198019fe54c2700e30010a0e35d8000eb88419fe588119fe5001084e50500a0e1"
     "16ffffeb000094e5106090e55d0f8fe20010a0e3568d00eb"),
    ("heap_module_list", 0x318,
     "1c008de2bb7900eb0050a0e1000055e30800000a58008fe28a8a00eb58108fe2a82200e3460f8fe2e88b00eb0500a0e1"
     "1d8000eb740000ea1c108de20c0084e2b07900eb"),
    ("heap_default_arguments", 0x4bc,
     "ae2200e31d0e8fe2878b00eb0500a0e1bc7f00eb130000ea100096e508008de5103084e208208de2f020cde1001096e5"
     "d820c6e10c0094e5f77e00eb0050a0e1"),
    ("default_heap_settings", 0x200a4,
     "70402de90040a0e10050a0e3000054e30400000a1420a0e384119fe50400a0e1563100eb000000ea0250a0e30500a0e1"
     "7080bde8"),
    ("heap_constructor", 0x200d8,
     "f84f2de90050a0e10190a0e102a0a0e10370a0e12c409de528609de50080a0e3000055e30700001a00f020e3ec081fe5"
     "140b00eb4e0f8fe2740c00eb00f020e30200a0e3f88fbde80720a0e10910a0e1150e8fe2a40000eb0500d6e5010050e3"
     "0700001a0730a0e10a20a0e10910a0e10400a0e100608de5c52300eb0080a0e1120000ea0500d6e5000050e30700001a"
     "0730a0e10a20a0e10910a0e10400a0e100608de5da2400eb0080a0e1070000ea00f020e374091fe5f20a00eb120e8fe2"
     "520c00eb00f020e30200a0e3dcffffea000094e5000050e31500000a080096e5001094e52c0081e50c0096e5001094e5"
     "480081e5100096e5001094e54c0081e5000094e5045080e5"),
    ("local_heap_map", 0x294ec,
     "ff4f2de90cd04de200b0a0e10180a0e10390a0e140609de50000a0e308008de5090088e004008de500a096e50400d6e5"
     "00008de50400d6e538129fe5001091e5010050e10200009a0200a0e308008de5780000ea0400d6e5030050e3080000da"
     "00f020e3760f8fe201e6ffeb08029fe561e7ffeb00f020e30200a0e308008de56c0000ea030088e20370c0e3641087e2"
     "04009de5010050e10200002a0400a0e308008de5630000ea0740a0e100708be50740a0e1647087e20000a0e3080084e5"
     "0c0084e5100084e5180084e51c0084e5200084e5240084e5288084e514009de5300084e50000a0e3500084e52c0084e5"
     "480084e54c0084e5349084e57c119fe500009de5000281e0380084e5380094e5080090e50001a0e13c0084e50500d6e5"
     "400084e50000a0e34400c4e5820f4fe2540084e5810f4fe2580084e5020c4fe25c0084e56d0f4fe2600084e502005ae3"
     "0000002a02a0a0e314a084e50100a0e3100aa0e1010040e2100084e5100094e5070080e0101094e50100c0e1180084e5"
     "101094e504009de50100c0e11c0084e5181094e5"),
    ("physical_to_virtual", 0x1fdac,
     "30402de90030a0e10140a0e10310a0e1280091e5040080e0305091e5050040e0000082e5000092e5185091e5050050e1"
     "0400003a000092e51c5091e5050050e10000008a1a0000ea040093e5000050e31500000a040093e5001090e5100000ea"
     "030051e10d00000a280091e5040080e0305091e5050040e0000082e5000092e5185091e5050050e10400003a000092e5"
     "1c5091e5050050e10000008a040000ea001091e5000051e3ecffff1a0200a0e33080bde800f020e30000a0e3fbffffea"),
    ("virtual_to_physical", 0x1fe6c,
     "30402de90030a0e10140a0e10310a0e1180091e5040050e10300008a1c0091e5040050e10000003a130000ea040093e5"
     "000050e30e00000a040093e5001090e5090000ea030051e10600000a180091e5040050e10300008a1c0091e5040050e1"
     "0000003a040000ea001091e5000051e3f3ffff1a0200a0e33080bde800f020e3300091e5040080e0285091e5050040e0"
     "000082e50000a0e3f6ffffea"),
    ("create_controller_config", 0x638,
     "0c008de28e3800eb"),
    ("controller_config_arguments", 0x7a0,
     "0c029fe518008de508029fe540008de504029fe544008de500029fe548008de50c308de2000094e5142090e5f420cde1"
     "00608de50030a0e3060090e8080084e2323800eb"),
    ("default_controller_config", 0xe87c,
     "10402de90040a0e14820a0e3141e9fe50400a0e19c4700eb0c0e9fe5200084e5080e9fe5000090e5240084e50000a0e3"
     "1080bde8"),
    ("controller_config_literals", 0xf6a4,
     "04dd0200f0fc0c0000fc0c00"),
    ("controller_factory", 0xe8b0,
     "f04f2de91cd04de20080a0e101a0a0e102b0a0e10390a0e148709de50040a0e30000a0e3000088e500f020e3780300e3"
     "0a4700eb0040a0e1000054e30200001a0300a0e31cd08de2f08fbde80200a0e300008de50050a0e300f020e3782300e3"
     "0010a0e30400a0e1714700eb000057e31f00000a4820a0e30710a0e11c0084e2744700eb0c0097e5"),
    ("controller_heap_fields", 0xea58,
     "180084e504b084e50000a0e36c03c4e540009de5100084e5340094e5000050e30200000a340094e5080084e5010000ea"
     "40009de5080084e5000059e30100000a0c9084e5010000ea080094e50c0084e5"),
    ("controller_initializers", 0xebb4,
     "0400a0e1866000eb0000a0e318008de50400a0e118109de5185900eb0c0294e5020050e30300003a0400a0e175feffeb"
     "0200a0e341ffffea100294e5030050e30300003a0400a0e16efeffeb0200a0e33affffea0400a0e1005f00eb0050a0e1"
     "000055e30300000a0400a0e165feffeb0500a0e131ffffea0400a0e1b66500eb0050a0e1000055e30300000a0400a0e1"
     "5cfeffeb0500a0e128ffffea0400a0e17f5e00eb0050a0e1000055e30300000a"),
    ("image_indices", 0x26dd8,
     "10402de90040a0e10000a0e39c00c4e50100a0e39d00c4e50400a0e39e00c4e5ec20a0e374109fe5a00084e2061600eb"
     "541194e5"),
    ("owned_image_allocation", 0x28310,
     "70402de968d04de20040a0e10060a0e30c0094e5081094e5010050e11400000a04108de20c0094e54fdeffeb04009de5"
     "ac0184e508009de5b00184e5ac0194e5000050e30200000ab00194e5000050e30200001a0400a0e368d08de27080bde8"
     "0000a0e3b801c4e50c009de5b40184e5150000eae23400e3312e8fe2f020cde10030a0e30c20a0e30116a0e30c0094e5"
     "8bdcffebac0184e5ac0194e5000050e30100001a0400a0e3eaffffea0c0094e5ac1194e51b2e84e2a7deffeb0100a0e3"
     "b801c4e50106a0e3b40184e50000a0e3dc0184e5fc0184e5f00184e5"),
    ("image_initialize", 0x26658,
     "70402de90050a0e1641095e50500a0e162ffffeb0040a0e1000054e30100000a0400a0e17080bde80500a0e1c20600eb"
     "0040a0e1000054e30100000a0400a0e1f7ffffea641095e50500a0e12bffffeb"),
    ("image_load_call", 0x26358,
     "f0412de90040a0e10160a0e1ac7194e5b08194e50610a0e10400a0e1350700eb0050a0e1000055e30100000a0500a0e1"
     "f081bde80400a0e14f0900eb"),
    ("combined_image_fallback", 0x27dc0,
     "f04f2de97cd04de20040a0e10170a0e10280a0e10390a0e1d0aacde10060a0e30050a0e30000a0e318008de53c5094e5"
     "400094e518008de59e00d4e514008de5b00194e510008de5ac0194e50c008de5b40194e508008de5003095e514209de5"
     "78108de218009de533ff2fe10060a0e1020056e30200001a0600a0e17cd08de2f08fbde8"),
    ("two_image_load", 0x28050,
     "70402de920d04de20040a0e10160a0e10000a0e314008de510008de5723f84e2072d84e2f020cde1713f84e26f2f84e2"
     "731f84e20400a0e14cffffeb0050a0e1000055e30000001a330000ea14308de200308de5b02194e59c10d4e51c308de2"
     "0400a0e1c5feffeb0050a0e1000055e30500001a14009de5000050e30200000a1c009de5000050e30200001a0500a0e1"
     "20d08de27080bde81c009de5bc0184e514009de5cc0184e5bc0194e514109de5000041e0c40184e510308de200308de5"
     "b02194e59d10d4e518308de20400a0e1aafeffeb0050a0e1000055e30500001a10009de5000050e30200000a18009de5"
     "000050e30100001a0500a0e1e3ffffea18009de5c00184e5c00194e510109de5000041e0c80184e500f020e30c208de2"
     "0c0094e5cc1194e50bdfffeb0c009de5940084e50c009de5980084e50000a0e3d2ffffea"),
    ("catalog_callbacks", 0x26e7c,
     "f0412de90080a0e10170a0e10250a0e10060a0e30040a0e3060055e30800003a00f020e3150e8fe2adefffeb0510a0e1"
     "530f8fe20cf1ffeb00f020e30200a0e3f081bde8056198e7000056e30100001a0200a0e3f9ffffea5c00a0e38ce5ffeb"
     "0040a0e1000054e30700001a00f020e3410f8fe29aefffeb460f8fe2faf0ffeb00f020e30300a0e3ecffffea5c20a0e3"
     "0010a0e30400a0e1f2e5ffeb040055e30100000a050055e30700001a5820a0e30400a0e1041096e5f3e5ffeb040096e5"
     "580080e2580084e5070000ea000096e5000090e5080084e5080096e5000090e50c0084e5040096e5580084e5004087e5"
     "0000a0e3d1ffffeaf0412de90070a0e10140a0e10250a0e10360a0e10780a0e1000054e30b00001a580056e30700000a"
     "00f020e350008fe26defffeb84008fe2cdf0ffeb00f020e30200a0e3f081bde8008085e5030000ea011044e2580098e5"
     "910620e0000085e50000a0e3f6ffffea10402de90040a0e1000054e30100000a0400a0e15de5ffeb1080bde8"),
    ("fallback_elf_loader", 0x27bd0,
     "ff4f2de91cd04de20050a0e101b0a0e103a0a0e10090a0e30000a0e318008de53c4095e5400095e514008de5b40195e5"
     "010650e30200002a0400a0e32cd08de2f08fbde80b20a0e110108de2003094e514009de533ff2fe10090a0e1000059e3"
     "0100000a0900a0e1f3ffffea5830a0e30c208de20010a0e304c094e510009de53cff2fe10090a0e1000059e30400000a"
     "081094e510009de531ff2fe10900a0e1e5ffffea0c009de5088090e50c009de50c0090e508008de5dc0195e5080050e1"
     "1100002a0800a0e11ce2ffeb0060a0e1000056e30400001a081094e510009de531ff2fe10400a0e3d3ffffea0820a0e1"
     "0010a0e30600a0e185e2ffeb0100a0e318008de50670a0e1030000ead46195e50000a0e318008de50670a0e110309de5"
     "00308de50020a0e30810a0e10700a0e1043094e56effffeb0090a0e1000059e30900000a081094e510009de531ff2fe1"
     "18009de5010050e30100001a0700a0e10de2ffeb0900a0e1b3ffffea081094e510009de531ff2fe124009de508109de5"
     "010080e000008ae550309de500209ae5f020cde10030a0e30820a0e10710a0e10500a0e16c0c00eb0090a0e118009de5"
     "010050e30100001a0700a0e1f6e1ffeb0900a0e19cffffea23232320000000004572726f72207265"),
    ("elf_loader", 0x2af2c,
     "ff4f2de914d04de20170a0e10280a0e10390a0e14ca09de50000a0e304008de508008de50c008de5500a01e36cd5ffeb"
     "0040a0e1000054e30800001a00f020e3380a1fe57adfffeb030c8fe2dae0ffeb00f020e30300a0e324d08de2f08fbde8"
     "0c009de5010080e20c008de5502a01e30010a0e30400a0e1ced5ffeb00b0a0e3049084e548009de5080084e5480a01e3"
     "047080e74c0a01e3048080e708308de20c208de20410a0e114009de562fdffeb000050e30900000a00f020e3b40a1fe5"
     "5bdfffebab0f8fe2bbe0ffeb00f020e30400a0e159d5ffeb60071fe5ddffffea0400a0e117fdffeb0400a0e1f0fcffeb"
     "0150a0e38f0000ea050185e0401084e2800181e0140090e5000050e30000001a870000ea050185e0401084e2800181e0"
     "080090e5020010e30000001a800000ea050185e0401084e2800181e0040090e5010050e30000000a790000ea050185e0"
     "401084e2800181e00c0090e5030250e30000003a760000ea08308de200308de50c308de20520a0e10410a0e114009de5"
     "67fcffeb000050e30200001a08029fe504008de56a0000ea0510a0e10400a0e1a2fcffeb0060a0e1000056e33300000a"
     "060186e0401084e2800181e0040090e5090050e30800001a00f020e3c40b1fe517dfffeb710f8fe277e0ffeb00f020e3"
     "e0019fe504008de5550000ea08308de200308de50c308de20620a0e10410a0e114009de546fcffeb000050e30800001a"
     "00f020e30c0c1fe505dfffeb6a0f8fe265e0ffeb00f020e36c019fe504008de5430000ea0620a0e10510a0e10400a0e1"
     "a0fbffeb00f020e3510d84e2060190e7000050e30800000a511d84e2060191e7f6d4ffeb0000a0e3511d84e2060181e7"
     "08009de5010080e208008de500f020e3050185e0401084e2800181e0080090e5040010e30500000a610d84e2050190e7"
     "081094e5010080e010008de5070000ea610d84e2050190e710008de500005be30200001a10009de500008ae501b0a0e3"
     "050185e0401084e2800181e0143090e5510d84e2052190e714009de510109de530faffeb00f020e3"),
    ("elf_endian", 0x2a5b8,
     "1100d4e5010050e30100000a0100a0e3000000ea0000a0e3000084e5"),
    ("section_destination", 0x2a474,
     "10402de90110a0e3270000ea0030a0e3614d80e2013184e7013181e0404080e2833184e00c2093e5000052e30600001a"
     "013181e0404080e2833184e0043093e5010053e30000000a160000ea030252e30000003a130000ea043090e5020053e1"
     "0000009a0f0000ea013181e0404080e2833184e0083093e5040013e30200000a043090e5032042e0030000ea043090e5"
     "033042e0084090e5042083e0613d80e2012183e700f020e3011081e2bc33d0e1010053e1d4ffffca1080bde8"),
    ("symbol_rebase", 0x2a3e0,
     "70402de90010a0e1405a01e3010095e70120a0e3190000ea100080e2be50d0e1000055e30200000abe50d0e1800055e3"
     "000000ba100000ea045090e5030255e30000003a0c0000eabe50d0e1055185e0406081e2854186e0045090e50c6094e5"
     "063045e0be50d0e1616d81e2055196e7035085e0045080e500f020e3012082e2445a01e3015095e7020055e1e1ffffca"
     "7080bde8"),
    ("relocation_selection", 0x2a35c,
     "10402de90020a0e10130a0e10110a0e3160000ea010181e0404082e2800184e01c0090e5030050e10000000a0e0000ea"
     "010181e0404082e2800184e0040090e5040050e30500000a010181e0404082e2800184e0040090e5090050e30100001a"
     "0100a0e11080bde800f020e3011081e2bc03d2e1010050e1e5ffffca0000a0e3f7ffffea"),
    ("apply_relocations", 0x29ff4,
     "f04f2de91cd04de20050a0e10160a0e10270a0e1060186e0401085e2800181e008008de5070187e0401085e2800181e0"
     "04008de504109de5140091e50c10a0e3fa0a00fb18008de5510d85e2074190e70000a0e314008de54e0000ea00f020e3"
     "000095e5000050e30a00000a000094e5200ca0e1001094e5ff1801e2210480e1001094e5ff1c01e2010480e1001094e5"
     "010c80e1000084e500f020e300f020e3000095e5000050e30a00000a040094e5200ca0e1041094e5ff1801e2210480e1"
     "041094e5ff1c01e2010480e1041094e5010c80e1040084e500f020e300f020e3000095e5000050e30a00000a080094e5"
     "200ca0e1081094e5ff1801e2410480e1081094e5ff1c01e2010480e1081094e5010c80e1080084e500f020e3040094e5"
     "ffa000e2040094e520b4a0e1001094e508009de50c0090e5008041e0510d85e2060190e7089080e0610d85e2060190e7"
     "080080e010008de5400a01e3050090e70b0280e00c008de5083094e500308de50c009de50a30a0e1042090e50900a0e1"
     "10109de589feffeb0c4084e214009de5010080e214008de5d401cde1010050e1adffffba1cd08de2"),
    ("vendor_relocation_dispatch_and_type4", 0x29ba4,
     "ff4f2de91cd04de20040a0e10280a0e150909de5096088e000a0a0e301b0a0e30200a0e318008de50300a0e314008de5"
     "0000a0e310008de50100a0e30c008de50200a0e308008de50000a0e304008de50100a0e300008de50050a0e328009de5"
     "0e0050e300f18f30b00000eaad0000ea0b0000ea0d0000ea130000ea1c0000ea260000ea480000ea730000ea7e0000ea"
     "810000ea880000ea920000ea9d0000ea9e0000ea00f020e30060c4e5aa0000ea00f020e304109de50160c4e75614e7e7"
     "00009de50010c4e7a30000ea00f020e310009de50060c4e75614e7e70c009de50010c4e75618e7e708009de50010c4e7"
     "990000ea00f020e30a60c4e75604e7e70b00c4e75618e7e718009de50010c4e7261ca0e114009de50010c4e78e0000ea"),
    ("vendor_relocation_exit", 0x29f00,
     "00f020e32cd08de2f08fbde8"),
    ("copy_loaded_section", 0x29ae4,
     "f8432de90080a0e10190a0e10240a0e10360a0e10d20a0e10910a0e1080098e5a8d8ffeb00709de50c0000ea0300d4e5"
     "001ca0e10200d4e5001881e10100d4e5001481e10000d4e5005081e1005087e5049089e2044084e2046046e2047087e2"
     "030056e3f0ffffca000056e3120000da0050a0e3047087e2010056e30900000a020056e30400000a030056e30800001a"
     "0200d4e5005885e100f020e30100d4e5005485e100f020e30000d4e5005085e100f020e300f020e3005087e5f883bde8"),
    ("owned_image_release", 0x2884c,
     "10402de90040a0e1d40194e5000050e30200000a080094e5d41194e566dcffebf40194e5000050e30200000a140094e5"
     "f41194e560dcffebec0194e5000050e30200000a100094e5e81194e55adcffebb801d4e5000050e30200000a0c0094e5"
     "ac1194e554dcffeb"),
    ("release_call", 0xe7a0,
     "0400a0e1286800eb"),
    ("packet_copy", 0x270ac,
     "fc20a0e30610a0e1940094e592e5ffeb181194e5cc2194e5"),
    ("init_literals", 0x5ea4,
     "64410d00"),
    ("init_output_literal", 0x5ed4,
     "f81f0d00"),
    ("global_context_literals", 0x6fc,
     "003a0d004c410d00"),
    ("heap_defaults_pointer", 0x20248,
     "1ce20200"),
    ("heap_defaults", 0x2e21c,
     "0000000003000000000000000000000000000000"),
    ("controller_defaults", 0x2dd04,
     "000000000000000000c2eb0bf8dc02000000000000000000000000000000000000000000000000000000000000000000"
     "000000000000000000000000000000000000000000000000"),
    ("catalog_descriptors_and_slots", 0xcfbb0,
     "78b30400d85d0500000009000000070000000200000005000000020000000000b0fb0c0060ea0200ccfb0c00b4fb0c00"
     "d89d0700b8fb0c00d0fb0c00dcfb0c0000000000000000000000000000000000e8fb0c00"),
    ("callback_table", 0xcfcf0,
     "7c6e0200746f0200dc6f0200"),
    ("outer_elf_header", 0x2ea60,
     "7f454c4601010100000000000000000002002d000100000078a6030034000000e0aa0400000000003400200012002800"
     "37003600"),
    ("inner_elf_header", 0x79dd8,
     "7f454c4601010100000000000000000002002d0001000000689f040034000000584c0500000000003400200013002800"
     "70006f00"),
    ("outer_section_order", 0x79568,
     "01000000010000000600000000000000d403000070000000000000000000000000040000010000000900000001000000"
     "060000000040000044040000d423000000000000000000000040000001000000230000000100000006000000d4630000"
     "18280000b81b0000000000000000000004000000010000003d00000001000000060000008c7f0000d0430000cc410000"
     "0000000000000000040000000100000059000000010000000600000058c100009c850000b04200000000000000000000"
     "04000000010000007500000001000000060000008c3f01004cc800006803000000000000000000000400000001000000"
     "980000000100000006000000f4420100b4cb0000c414000000000000000000000400000001000000bd00000001000000"
     "06000000b857010078e000000422000000000000000000000400000001000000d60000000100000006000000bc790100"
     "7c0201007448000000000000000000000400000001000000f100000001000000060000008cff0100f04a010074030000"
     "0000000000000000040000000100000013010000010000000600000000030200644e0100a41600000000000000000000"
     "0400000001000000370100000100000006000000d423020008650100a005000000000000000000000400000001000000"
     "50010000010000000600000074290200a86a01000c040000000000000000000004000000010000006a01000001000000"
     "06000000802d0200b46e01002c0d000000000000000000000400000001000000830100000100000006000000ac3a0200"
     "e07b0100c802000000000000000000000400000001000000a00100000100000006004000743d0200a87e010054760100"
     "00000000000000000400000001000000ae010000010000000300000000000700fcf40200000100000000000000000000"
     "0004000001000000"),
    ("inner_section_order", 0xcea58,
     "01000000010000000600000000000000f403000070000000000000000000000000040000010000000900000001000000"
     "060000000020000064040000e003000000000000000000000020000001000000230000000100000006000000e0230000"
     "440800007c080000000000000000000004000000010000003700000001000000060000005c2c0000c01000003c180000"
     "000000000000000004000000010000004b000000010000000600000098440000fc280000540e00000000000000000000"
     "0400000001000000650000000100000006000000ec520000503700001c0d000000000000000000000400000001000000"
     "7c0000000100000006000000086000006c44000038010000000000000000000004000000010000004105000008000000"
     "0300000040610000a445000000000000000000000000000001000000010000009a000000010000000600000008800000"
     "a44500002001000000000000000000000400000001000000b7000000010000000600000040810000c44600002c000000"
     "00000000000000000400000001000000d200000001000000060000005c8c0000f0460000fc1100000000000000000000"
     "0400000001000000e70000000100000006000000589e0000ec580000600b000000000000000000000400000001000000"
     "020100000100000006000000b8a900004c6400007c090000000000000000000004000000010000001a01000001000000"
     "06000000e0c30000c86d00008c1d0000000000000000000004000000010000002c01000001000000060000006ce10000"
     "548b0000281000000000000000000000040000000100000041010000010000000600000094f100007c9b000060040000"
     "000000000000000004000000010000005f0100000100000006000000f4f50000dc9f00008c0100000000000000000000"
     "04000000010000007b01000001000000060000000000010039d90000a807000000000000000000000020000001000000"
     "8501000001000000060000000020010039f900002407000000000000000000000020000001000000a201000001000000"
     "060000000040010039190100f80a000000000000000000000020000001000000bf010000010000000600000000600100"
     "39390100881c000000000000000000000020000001000000dc01000001000000060000000080010039590100b01b0000"
     "00000000000000000020000001000000f9010000010000000600000000a00100397901005c1e00000000000000000000"
     "002000000100000016020000010000000600000000c0010039990100f81e000000000000000000000020000001000000"
     "33020000010000000600000000e0010029ba0100f0040000000000000000000000200000010000003d02000001000000"
     "060000000000020029da0100700d0000000000000000000000200000010000005f020000010000000600000000200200"
     "29fa0100381b00000000000000000000002000000100000081020000010000000600000000400200291a0200341a0000"
     "00000000000000000020000001000000a1020000010000000600000000600200293a0200b41c00000000000000000000"
     "0020000001000000be020000010000000600000000800200295a02007019000000000000000000000020000001000000"
     "db020000010000000600000000a00200297a02005417000000000000000000000020000001000000f802000001000000"
     "0600000000c00200299a02001c1800000000000000000000002000000100000015030000010000000600000000e00200"
     "29ba0200b41c0000000000000000000000200000010000003203000001000000060000000000030029da0200ec100000"
     "000000000000000000200000010000004f03000001000000060000000020030029fa0200bc1800000000000000000000"
     "00200000010000006c030000010000000600000000400300291a0300241f000000000000000000000020000001000000"
     "89030000010000000600000000600300293a0300741d000000000000000000000020000001000000a603000001000000"
     "06000000747d03009d570300840e000000000000000000000400000001000000c30300000100000006000000f88b0300"
     "21660300e014000000000000000000000400000001000000d90300000100000006000000d8a00300017b0300741b0000"
     "00000000000000000400000001000000f903000001000000060000004cbc030075960300180000000000000000000000"
     "04000000010000000d040000010000000600000064bc03008d9603004015000000000000000000000400000001000000"
     "2e0400000100000006000000a4d10300cdab0300701e0000000000000000000004000000010000004f04000001000000"
     "0600000014f003003dca0300541300000000000000000000040000000100000065040000010000000600000068030400"
     "91dd0300bc040000000000000000000004000000010000007a0400000100000006000000240804004de2030044070000"
     "00000000000000000400000001000000980400000100000006004000680f040091e90300a89200000000000000000000"
     "04000000010000009e040000010000000600000010a20400397c0400dc02000000000000000000000400000001000000"
     "5c05000008000000030000000000050068a100000000000000000000000000000004000001000000ab04000001000000"
     "020000000000050068a100008001000000000000000000000004000001000000"),
    ("outer_symbol_strings_header", 0x79a90,
     "8002000003000000000000000000000035900300da20000000000000000000000100000001000000"),
    ("outer_symbol_table_header", 0x79ab8,
     "8802000002000000000000000000000010b1030050340000220000000d0200000400000010000000"),
    ("outer_rela_text_header", 0x79d38,
     "7a040000040000000000000000000000203d0400c46500002300000010000000040000000c000000"),
    ("outer_section_strings_header", 0x79db0,
     "9d04000003000000000000000000000038a60400a704000000000000000000000100000001000000"),
    ("inner_section_strings_header", 0xcfb88,
     "c60a000003000000000000000000000088410500d00a000000000000000000000100000001000000"),
    ("packet_section_symbol", 0x69c90,
     "00000000000007000001000003001100"),
    ("outer_command_symbol", 0x6be10,
     "0c0e0000085802000402000012001000"),
    ("outer_dma_read_symbol", 0x6c3d0,
     "8f140000c85300004400000012000200"),
    ("outer_command_code", 0x4839c,
     "043e0e1000360e1000386e63207e8e53101a0e10141c0e10181e0e10007cff61001d0530007cdf6100010700007fa741"
     "009a0660009e2760a070bf2f80fe5f402063bf2f1c200e1000800708007de05702ff767301210020007de05701ff7673"
     "011e0020007c20500100767308fae0578d27002000a01f08037e00400002004000000038000400200005002000060020"
     "00070020000800200009002000120020001300200014002020a7fd2f009e07608020002020d9fd2f009e0760001f0020"
     "205dfe2f009e0760801d0020206dfe2f009e0760001c00202099fe2f009e0760801a002000041f086ad3ff3f0001e067"
     "82020020007c1f607826070080c6fc2f2016002001fe1f60a0510228048007080001006202020020007c1f60a4260700"
     "a073032804802708a011002004a007102073ff2f009e0760800f0020a08bff2f009e0760000e002020b5ff2f009e0760"
     "800c0020a0c8ff2f009e0760000b002004800708017ae057007c1f6270cdff3f0206002013040808007ae0570c020020"
     "800bbd2f13040808007ae05709feff2780d6c72f01fe1f6021004810a002002000000050000000502001002021004810"
     "ffff1f6004800710007c1f60001d0530007f2740a035bf2f80fe5f408031bf2f80d6c42f007c1f60000f003080402008"
     "841a0014807acc2f1000ae091400ce091800ee090480ed0b1c000e0a20800f3820106e0b"),
    ("outer_dma_read_code", 0x3026c,
     "007c9f6000180530404062080cfe61600cfae15701feff274040620804fae16702020020200002142402021428040214"
     "00800f3830000214340202143804021400800f38"),
    ("packet_pointer_relocation", 0x72f00,
     "305802000412000000010000"),
    ("outer_text_name", 0x79238,
     "2e7465787400"),
    ("outer_packet_name", 0x79246,
     "2e766465635f636d645f626c6f636b00"),
    ("outer_rela_name", 0x79512,
     "2e72656c612e7465787400"),
    ("outer_command_name", 0x688a1,
     "436f72655f436f6d6d616e6400"),
    ("outer_dma_read_name", 0x68f24,
     "446d615f5265616400"),
    ("inner_first_data_name", 0xce40b,
     "2e726f6461746100"),
    ("host_command_base", 0x6174,
     "08317673"),
    ("heap_entry", 0x1dc,
     "70402de920d04de20060a0e10140a0e11820a0e30010a0e30400a0e1398100eb518000eb0050a0e1"),
    ("heap_settings_call", 0x490,
     "0c1094e5750f8fe2c97f00eb08008de2ff7e00eb0050a0e1000055e30800000a4b0f4fe2298a00eb4b1f4fe2"),
    ("host_dispatch_entry", 0x5f2c,
     "70402de90040a0e17b0f8fe2216900eb000054e32300000a"),
    ("local_heap_completion", 0x29680,
     "0400a0e166ffffeb0050a0e11c0094e5181094e5010040e0381094e5081091e50111a0e1941081e2382094e5082092e5"
     "0430a0e3931221e0010050e10c00009a1c0094e5181094e5010040e0000085e50000a0e3040085e5080085e5900085e5"
     "0510a0e10400a0e10ad5ffeb085084e5010000ea0600a0e308008de5380094e50e00d0e5000050e30400000a08009de5"
     "000050e30100000a0400a0e172d7ffeb00f020e308009de51cd08de2f08fbde8"),
    ("vendor_relocation_type6", 0x29d50,
     "00f020e320009de5000046e0047040e2030017e30600000a00f020e3660f8fe2fae3ffeb790f8fe220109de559e5ffeb"
     "00f020e3c70ab0e10900000a0000e0e3c70a50e10600000a00f020e35a0f8fe2eee3ffeb7b0f8fe220109de54de5ffeb"
     "00f020e38772a0e13e73c7e37f70c7e30000d4e57f0000e20310d4e5f81001e2015c80e0075085e10a50c4e75504e7e7"
     "0b00c4e75518e7e718009de50010c4e7250ca0e114109de50100c4e73f0000ea"),
    ("outer_dma_sync_symbol", 0x6c3b0,
     "7c140000645300001800000012000200"),
    ("outer_dma_sync_name", 0x68f11,
     "446d615f53796e6300"),
    ("outer_dma_sync_code", 0x30208,
     "007c3f600018053040c000080f7ae06701800f3800feff27"),
    ("outer_call_relocations", 0x72f0c,
     "405802000686020000000000485802000684020000000000"),
)
_COMMAND_BUFFER_BRIDGE_RELA_SHA256 = "8c1c3eb2f61ad26f9278028b0aa9a596650345df13b1dd0d6d806cf677b9bba9"
DEFAULT_SYMBOLS = (
    "Arc_UartInit", "Arc_UartPoll", "ArcGetc", "ArcPutc", "ArcCommandBuffer",
    "ReadLine", "MatchKeyword", "Core_Command", "CmdPeek", "CmdCore",
    "CmdState", "CmdTrace", "CmdCabac",
)


class FormatError(ValueError):
    pass


def read_firmware(path):
    # O_PATH does not invoke a device's open handler. Pin and check the inode
    # before reopening it for reading, so a pathname race cannot open hardware.
    reference = os.open(path, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        metadata = os.fstat(reference)
        if not stat.S_ISREG(metadata.st_mode):
            raise FormatError("firmware must be a regular file, not a device or symlink")
        if not 24 <= metadata.st_size <= MAX_FIRMWARE_SIZE:
            raise FormatError("firmware size is outside the BCM70015 download bounds")
        descriptor = os.open(f"/proc/self/fd/{reference}", os.O_RDONLY | os.O_CLOEXEC)
        try:
            source = os.fdopen(descriptor, "rb")
        except BaseException:
            os.close(descriptor)
            raise
        with source:
            data = source.read(MAX_FIRMWARE_SIZE + 1)
        if len(data) != metadata.st_size:
            raise FormatError("firmware changed size while reading")
        return data
    finally:
        os.close(reference)


def bounded(data, offset, size, description):
    if offset < 0 or size < 0 or offset > len(data) - size:
        raise FormatError(f"{description} extends outside the firmware payload")
    return data[offset:offset + size]


def _arc_comment(data, base, expected_count):
    """Validate stored compiler hints, not the meaning of vendor core options."""
    if not data or len(data) > MAX_ARC_COMMENT_BYTES or data[-1] != 0:
        raise FormatError("invalid ARC comment size or termination")
    if data.count(b"\0") > MAX_ARC_COMMENT_RECORDS:
        raise FormatError("ARC comment record budget exceeded")
    records = data[:-1].split(b"\0")
    if any(not record or len(record) > MAX_ARC_METADATA_STRING_BYTES or
           any(byte < 32 or byte > 126 for byte in record.replace(b"\n", b""))
           for record in records):
        raise FormatError("invalid ARC comment record")
    linker = b"MetaWare Linker v8.4.6\n"
    compiler = b"hc8.4.18 -a4 -core8 -O -Xbs "
    if not records[0].startswith(linker):
        raise FormatError("ARC linker hint does not match the baseline")
    records[0] = records[0][len(linker):]
    if len(records) != expected_count * 2:
        raise FormatError("ARC compiler record count does not match the baseline")
    for index in range(0, len(records), 2):
        if (not records[index].startswith(compiler) or
                not re.fullmatch(rb"[A-Za-z_][A-Za-z_0-9]*\.c", records[index][len(compiler):]) or
                records[index + 1] != b"11202009.075353"):
            raise FormatError("ARC compiler hint record does not match the baseline")
    return {"linker_hint": linker[:-1].decode("ascii"),
            "compiler_hint": compiler.rstrip().decode("ascii"),
            "compiler_record_count": expected_count, "nul_record_count": len(records),
            "first_compiler_record_blob_file_offset": base + len(linker)}


def _arc_extensions(data, base):
    """Strict declarations only; GNU 2.23.2 arc-ext.c:159-172 defines the layout."""
    if not data or len(data) > MAX_ARC_EXTENSION_BYTES:
        raise FormatError("ARC extension byte budget exceeded")
    records, identities, names = [], set(), set()
    offset = 0
    while offset < len(data):
        if len(records) >= MAX_ARC_EXTENSION_RECORDS:
            raise FormatError("ARC extension record budget exceeded")
        if len(data) - offset < 2:
            raise FormatError("truncated ARC extension record header")
        length, kind = data[offset:offset + 2]
        if kind not in (0, 2):
            raise FormatError("unsupported ARC extension record type")
        prefix = 5 if kind == 0 else 6
        if length < prefix + 2 or length > len(data) - offset:
            raise FormatError("invalid ARC extension record length")
        record = data[offset:offset + length]
        name = record[prefix:]
        if (name[-1] != 0 or b"\0" in name[:-1] or
                len(name) - 1 > MAX_ARC_METADATA_STRING_BYTES or
                any(byte < 32 or byte > 126 for byte in name[:-1])):
            raise FormatError("invalid ARC extension name")
        name = name[:-1].decode("ascii")
        item = {"record_blob_file_offset": base + offset, "length": length,
                "type": kind, "name": name}
        if kind == 0:
            opcode, minor, flags = record[2:5]
            if not 0x10 <= opcode <= 0x1f or minor or flags:
                raise FormatError("ARC extension instruction fields do not match the baseline")
            item.update(opcode=opcode, minor_opcode=minor, flags=flags)
            identity = (kind, opcode, minor)
        else:
            address = int.from_bytes(record[2:6], "big")
            item["auxiliary_address"] = address
            identity = (kind, address)
        if identity in identities or name in names:
            raise FormatError("duplicate ARC extension declaration")
        identities.add(identity)
        names.add(name)
        records.append(item)
        offset += length
    expected = [(2, 0x21, "t0_count"), (2, 0x22, "t0_control"), (2, 0x23, "t0_limit")]
    expected += [(0, opcode, name) for opcode, name in
                 ((0x10, "asl"), (0x11, "lsr"), (0x12, "asr"), (0x13, "ror"),
                  (0x16, "mul16"), (0x1e, "max"), (0x1f, "min"))]
    if [(r["type"], r.get("opcode", r.get("auxiliary_address")), r["name"])
            for r in records] != expected:
        raise FormatError("ARC extension declarations do not match the baseline")
    return records


def _arc_metadata_map(payload, images, image_sections):
    """Private pure validator; public entry requires the exact bundled SHA/size."""
    extents = [(0x2ea60, 0x79dd8), (0x79dd8, 0xcfbb0)]
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or len(image_sections) != 2 or
            [(i["blob_file_offset"], i["blob_file_end"]) for i in images] != extents):
        raise FormatError("ARC metadata image identities do not match the baseline")
    expected = ((32, 0x6711c, 2313, 0x79a40, 0x67a25, 0x79a68, 43),
                (63, 0xc1ced, 1289, 0xcf408, 0xc21f6, 0xcf430, 24))
    result, extensions, budget = [], [], MAX_ARC_METADATA_BYTES
    for image, sections, values in zip(images, image_sections, expected):
        if (image["class"], image["endianness"], image["machine"],
                image["elf_type"], image["flags"]) != (32, "little", 45, 2, 0):
            raise FormatError("ARC ELF header does not match the baseline")
        if len(sections) != 2 or {s["name"] for s in sections} != {".comment", ".arcextmap"}:
            raise FormatError("missing, duplicate or unexpected ARC metadata section")
        index, comment_base, comment_size, comment_header, ext_base, ext_header, count = values
        contents, sources = {}, {}
        for name, position, size, section_index, header in (
                (".comment", comment_base, comment_size, index, comment_header),
                (".arcextmap", ext_base, 112, index + 1, ext_header)):
            section = next(s for s in sections if s["name"] == name)
            if (section["size"] > budget or section["size"] >
                    (MAX_ARC_COMMENT_BYTES if name == ".comment" else MAX_ARC_EXTENSION_BYTES)):
                raise FormatError("ARC metadata byte budget exceeded")
            budget -= section["size"]
            if (section["blob_file_offset"] is None or section["size"] < 0 or
                    not image["blob_file_offset"] <= section["blob_file_offset"] <=
                    image["blob_file_end"] - section["size"]):
                raise FormatError("ARC metadata section crosses its ELF image bounds")
            if (section["section_index"], section["type"], section["flags"],
                    section["elf_virtual_address"], section["blob_file_offset"], section["size"],
                    section["link"], section["info"], section["align"], section["entry_size"],
                    section["section_header_blob_file_offset"]) != (
                        section_index, 1, 0, 0, position, size, 0, 0, 1, 1, header):
                raise FormatError("ARC metadata section does not match the baseline")
            contents[name] = bounded(payload, position, size, "ARC metadata section")
            sources[name] = {"blob_file_offset": position, "size": size,
                             "section_header_blob_file_offset": header}
        declarations = _arc_extensions(contents[".arcextmap"], ext_base)
        extensions.append(contents[".arcextmap"])
        result.append({"image_blob_file_offset": image["blob_file_offset"],
                       "elf_flags": image["flags"], "gnu_2_23_2_flag_machine": "ARC5",
                       "arc_attributes_present": False, "sections": sources,
                       "comment": _arc_comment(contents[".comment"], comment_base, count),
                       "extension_declarations": declarations})
    if extensions[0] != extensions[1]:
        raise FormatError("ARC extension maps differ between the baseline images")
    return {"gnu_binutils_version": "2.23.2", "architecture_selection": "unresolved",
            "extension_record_layout_source": "opcodes/arc-ext.c:159-172; gas/config/tc-arc.c:609-623,824-837",
            "elf_flags_source": "include/elf/arc.h:42-49; bfd/elf32-arc.c:187-205",
            "auxiliary_address_byte_order": "big", "extension_maps_identical": True,
            "images": result, "limitations": [
                "MetaWare -core8 and GNU ELF flag interpretations use different namespaces; their relationship is unresolved.",
                "Extension records declare ASCII names and fields, not instruction semantics or a complete ISA.",
                "Stored metadata does not establish silicon architecture, a call graph or codec capabilities."]}


def _inner_dispatch_map(payload):
    """Fixed byte-selector projection; no device access or ISA/runtime certificate."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("inner-dispatch payload size does not match the baseline")
    regions = _INNER_DISPATCH_REGIONS
    total = sum(size for _, _, size, _ in regions)
    if (len(regions) > MAX_INNER_DISPATCH_REGIONS or total > MAX_INNER_DISPATCH_BYTES or
            636 // 12 > MAX_INNER_DISPATCH_RELOCATIONS):
        raise FormatError("inner-dispatch validation budget exceeded")
    contents, positions, validated = {}, {}, []
    # Complete selected code and RELA table are pinned BEFORE any interpretation.
    for role, offset, size, digest in regions:
        raw = bounded(payload, offset, size, "inner-dispatch region")
        if hashlib.sha256(raw).hexdigest() != digest:
            raise FormatError(f"inner-dispatch region {role} does not match the baseline")
        contents[role], positions[role] = raw, offset
        validated.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": digest})
    base, end = 0x79dd8, 0xcfbb0
    header = struct.unpack("<16sHHIIIIIHHHHHH", contents["inner_header"])
    sections = {index: struct.unpack("<10I", contents[f"section_{index}"])
                for index in (2, 3, 4, 15, 18, 25, 47, 65, 66, 69)}
    for index, section in sections.items():
        if (positions[f"section_{index}"] != base + header[6] + index * 40 or
                base + section[4] + section[5] > end):
            raise FormatError("inner-dispatch section ownership does not match the baseline")
    code, symbols, names, rela = (sections[index] for index in (3, 66, 65, 69))
    if (code[1:4] != (1, 6, 0x23e0) or code[5] != len(contents["core_code"]) or
            base + code[4] != positions["core_code"] or
            symbols[1] != 2 or symbols[6] != 65 or symbols[9] != 16 or names[1] != 3 or
            rela[1] != 4 or rela[6:10] != (66, 3, 4, 12) or
            base + rela[4] != positions["core_relocations"] or
            rela[5] != len(contents["core_relocations"])):
        raise FormatError("inner-dispatch code/symbol/RELA ownership is incoherent")
    records = [struct.unpack_from("<IIi", contents["core_relocations"], i)
               for i in range(0, rela[5], 12)]

    def word(address):
        delta = address - code[3]
        if delta < 0 or delta + 4 > code[5]:
            raise FormatError("inner-dispatch instruction is outside selected code")
        return struct.unpack_from("<I", contents["core_code"], delta)[0]

    def branch_target(address, link):
        instruction = word(address)
        if instruction & 0xf8000000 != (0x28000000 if link else 0x20000000):
            raise FormatError("inner-dispatch selected branch opcode does not match")
        displacement = (instruction >> 7) & 0xfffff
        if displacement & 0x80000:
            displacement -= 0x100000
        return address + 4 + displacement * 4

    # Under STATUS next-word-PC semantics: lr@28C8 yields (PC+4)/4,
    # then +3+selector targets the ten branch words at 28D8..28FC.
    table = []
    labels = ("H264/H264P", "MPEG", "diagnostic", "H263", "VC1", "diagnostic",
              "diagnostic", "diagnostic", "MP4", "AVS")
    expected = (0x2900, 0x2954, 0x29e4, 0x29c8, 0x2974,
                0x29e4, 0x29e4, 0x29e4, 0x29ac, 0x2990)
    for selector, (label, target) in enumerate(zip(labels, expected)):
        address = 0x28d8 + 4 * selector
        if branch_target(address, False) != target:
            raise FormatError("inner-dispatch selected table target does not match")
        table.append({"internal_selector": selector, "table_elf_virtual_address": address,
                      "target_elf_virtual_address": target, "selected_path": label})
    # No original relocation touches the local context call or selector sequence.
    if (branch_target(0x2868, True) != 0x23e0 or
            any(a == 0x2868 or 0x28bc <= a < 0x2900 for a, _, _ in records)):
        raise FormatError("inner-dispatch local/table relocation inventory differs")
    calls = []
    for role, address, target in (
            ("prefix_dma", 0x2840, 0x2160), ("h264p", 0x291c, 0x43834),
            ("h264", 0x2938, 0x2e3c), ("mpeg", 0x2958, 0x441e4),
            ("vc1", 0x2974, 0x1e1e8), ("avs", 0x2990, 0x10000),
            ("mp4", 0x29ac, 0x45e04), ("h263", 0x29c8, 0xe97c)):
        matches = [(index, record) for index, record in enumerate(records) if record[0] == address]
        if len(matches) != 1:
            raise FormatError("inner-dispatch selected call relocation is not unique")
        index, record = matches[0]
        symbol = struct.unpack("<IIIBBH", contents[f"{role}_symbol"])
        destination = sections[symbol[5]]
        if (record[1] & 255 != 6 or record[2] != 0 or
                base + symbols[4] + (record[1] >> 8) * 16 != positions[f"{role}_symbol"] or
                symbol[1] != target or symbol[3:5] != (0x12, 0) or
                destination[1] != 1 or not destination[2] & 4 or
                not destination[3] <= target < target + symbol[2] <= destination[3] + destination[5] or
                base + names[4] + symbol[0] != positions[f"{role}_name"] or
                symbol[0] + len(contents[f"{role}_name"]) > names[5] or
                branch_target(address, True) != target):
            raise FormatError("inner-dispatch selected call/symbol ownership differs")
        delta = target + record[2] - address - 4
        original = word(address)
        patched = (original & 0xf800007f) | ((delta << 5) & 0x07ffff80)
        if delta & 3 or not -(1 << 21) <= delta < (1 << 21) or patched != original:
            raise FormatError("inner-dispatch fixed type-6 arithmetic differs")
        calls.append({"target": contents[f"{role}_name"][:-1].decode("ascii"),
                      "call_elf_virtual_address": address, "target_elf_virtual_address": target,
                      "relocation_record_blob_file_offset": positions["core_relocations"] + index * 12,
                      "symbol_record_blob_file_offset": positions[f"{role}_symbol"],
                      "vendor_type": 6, "addend": 0, "original_word": original,
                      "word_preserved_under_uniform_code_translation": True})
    return {
        "device_observed": False, "basis": "pinned original inner ELF bytes",
        "interpretation": "conditional GNU ARC base model; STATUS next-word-PC and uniform code translation assumed",
        "validated_regions": validated, "validated_byte_count": total,
        "relocation_record_count": len(records),
        "prefix_context_link": {
            "packet_codec_byte_offset": 0, "packet_channel_byte_offset": 4,
            "prefix_local_destination": 0x3fffc014, "prefix_copy_bytes": 48,
            "cached_channel_local_address": 0x3fffc094, "cached_codec_local_address": 0x3fffc095,
            "comparison_load_elf_virtual_addresses": [0x2848, 0x284c, 0x2858, 0x285c],
            "mismatch_call_elf_virtual_address": 0x2868, "context_entry_elf_virtual_address": 0x23e0,
            "new_channel_load_elf_virtual_address": 0x2764,
            "new_codec_load_elf_virtual_address": 0x2768,
            "cache_store_elf_virtual_addresses": [0x276c, 0x2774],
            "explicit_failure_status_protocol_validated": False,
            "caller_checks_context_status": False, "cache_update_proves_restore_success": False,
            "same_context_generation_validated": False,
            "invalid_channel_ge_16_path_elf_virtual_address": 0x275c,
            "unsupported_restore_path_elf_virtual_address": 0x2748},
        "selector": {
            "source": "cached codec byte, not a direct packet load or packet+184 SiU byte",
            "load_elf_virtual_address": 0x28bc, "local_address": 0x3fffc095,
            "unsigned_upper_bound": 9, "above_bound_target_elf_virtual_address": 0x29e4,
            "status_read_elf_virtual_address": 0x28c8, "status_add_word_count": 3,
            "table_first_elf_virtual_address": 0x28d8, "table_bytes": 40,
            "byte_domain_size": 256, "table": table,
            "selected_h264_subselection_elf_virtual_addresses": [0x2900, 0x2910, 0x291c, 0x2938],
            "host_open_enum_equivalence_validated": False,
            "diagnostic_is_hardware_codec_rejection": False},
        "selected_calls": calls,
        "limitations": [
            "Selected static branch/type-6 arithmetic does not validate vendor ISA, runtime overlays or applied relocation.",
            "Prefix DMA visibility, saved/restored codec state and opaque logging/MMIO/callee completion remain assumptions.",
            "Invalid channel and unsupported restore paths can still update the cache if their opaque callees return.",
            "The caller does not check context-switch status; cached selector equality is not successful context restoration.",
            "The context predicate compares only channel/selector bytes, not generation or retained-frame ownership.",
            "Internal selector paths do not establish host OPEN IDs, codec support or a parser-independent backend API."]}


def _inner_descriptor_map(payload, images):
    """Two fixed pre-relocation field paths, conditional on the base GNU ARC model."""
    identities = [(0x2ea60, 0x79dd8, 55), (0x79dd8, 0xcfbb0, 112)]
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            [(i.get("blob_file_offset"), i.get("blob_file_end"), i.get("section_count"))
             for i in images] != identities or
            any((i.get("class"), i.get("endianness"), i.get("machine"),
                 i.get("elf_type"), i.get("flags")) != (32, "little", 45, 2, 0)
                for i in images)):
        raise FormatError("inner-descriptor image identities do not match the baseline")
    regions = [(f"elf_header_{slot}", offset, bytes.fromhex(raw))
               for slot, offset, raw in _INNER_DESCRIPTOR_HEADERS]
    for slot, index, offset, raw, name_offset, name in _INNER_DESCRIPTOR_SECTIONS:
        regions.append((f"section_header_{slot}_{index}", offset, bytes.fromhex(raw)))
        if name != ".shstrtab":
            regions.append((f"section_name_{slot}_{index}", name_offset, name.encode("ascii") + b"\0"))
    regions.extend((role, offset, bytes.fromhex(raw))
                   for _, role, _, _, offset, raw in _INNER_DESCRIPTOR_WINDOWS)
    if (len(regions) > MAX_INNER_DESCRIPTOR_REGIONS or
            sum(len(raw) for _, _, raw in regions) > MAX_INNER_DESCRIPTOR_BYTES):
        raise FormatError("inner-descriptor validation budget exceeded")
    validated = []
    # All selected bytes, including headers, literals and delay slots, precede
    # any field interpretation. These are ORIGINAL bytes, not loaded operands.
    for role, offset, expected in regions:
        actual = bounded(payload, offset, len(expected), "inner-descriptor region")
        if actual != expected:
            raise FormatError(f"inner-descriptor region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": len(actual),
                          "sha256": hashlib.sha256(actual).hexdigest()})
    headers = {slot: struct.unpack("<16sHHIIIIIHHHHHH", bytes.fromhex(raw))
               for slot, _, raw in _INNER_DESCRIPTOR_HEADERS}
    sections = {(slot, index): struct.unpack("<10I", bytes.fromhex(raw))
                for slot, index, _, raw, _, _ in _INNER_DESCRIPTOR_SECTIONS}
    for slot, index, offset, _, name_offset, name in _INNER_DESCRIPTOR_SECTIONS:
        base, end, _ = identities[slot]
        header, section = headers[slot], sections[slot, index]
        names = sections[slot, header[-1]]
        if (offset != base + header[6] + index * 40 or names[1] != 3 or
                base + names[4] + names[5] > end or
                name_offset != base + names[4] + section[0] or
                section[0] + len(name) + 1 > names[5] or
                base + section[4] + section[5] > end):
            raise FormatError("inner-descriptor section mapping does not match the baseline")
    windows = []
    for slot, role, index, address, offset, raw in _INNER_DESCRIPTOR_WINDOWS:
        base, _, _ = identities[slot]
        section = sections[slot, index]
        size = len(raw) // 2
        if (section[1] != 1 or not section[2] & 4 or address < section[3] or
                address + size > section[3] + section[5] or
                offset != base + section[4] + address - section[3]):
            raise FormatError("inner-descriptor instruction mapping does not match the baseline")
        windows.append({"role": role, "image_slot": slot, "section_index": index,
                        "elf_virtual_address": address, "blob_file_offset": offset, "size": size})
    return {
        "device_observed": False, "basis": "original pre-relocation ELF bytes",
        "address_domain": "ELF virtual addresses and ARC-local firmware literals, not host addresses",
        "interpretation": {"tool": "GNU binutils 2.23.2 default ARC base-case model",
                           "two_operand_asl_is_add_alias_source": "opcodes/arc-opc.c:1365-1366"},
        "paths": {
            "record_pointer_and_boundary": {
                "core_state_local_base": 0x3fffcf70,
                "pointer_state_offset": 120, "boundary_state_offset": 124,
                "allocator_publish_elf_virtual_address": 0x888c,
                "allocator_success_return_value": 0,
                "mpeg_allocator_call_elf_virtual_address": 0x2daa4,
                "mpeg_allocator_delay_slot_elf_virtual_address": 0x2daa8,
                "record_F": {"core_prefix_offset": 92, "prefix_bytes": 56, "stride_bytes": 284,
                             "pointer_P_offset": 28, "boundary_offset": 32,
                             "producer_record_base_core_state_offset": 88,
                             "reader_record_base_local_address": 0x3fffd2dc,
                             "completion_record_base_local_address": 0x3fffd2dc,
                             "same_record_pool_identity_validated": False},
                "record_pool_context_snapshot": {
                    "immediate_slot_assignment": {
                        "context_argument_elf_virtual_address": 0x2672c,
                        "source_slot_write_elf_virtual_address": 0x267b0,
                        "source_slot_context_offset": 0x530,
                        "source_reload_elf_virtual_address": 0x268c8,
                        "destination_address_calculation_elf_virtual_address": 0x268cc,
                        "destination_slot_context_offset": 0x21c,
                        "destination_store_elf_virtual_address": 0x268dc,
                        "equal_value_immediately_after_store_under_base_model": True,
                        "scope": "loaded value assignment only; conditional on unchanged source during this edge",
                        "constructor_exit_equality_validated": False},
                    "conditional_constructor_return": {
                        "source_initialization": {
                            "context_end_offset": 0x5bc,
                            "size_call_elf_virtual_addresses": [0x26760, 0x26768],
                            "original_size_callee_elf_virtual_address": 0x3b304,
                            "size_return_delay_slot_elf_virtual_address": 0x3b308,
                            "size_return_word": 0x401ffeec,
                            "add_short_immediate_operands": [236, 236],
                            "size_bytes_under_base_model": 0x1d8,
                            "pool_context_offset_under_conditions": 0x794,
                            "earlier_variable_loop": {
                                "body_elf_virtual_address": 0x2689c,
                                "constructor_argument_register": 14,
                                "store_context_offsets": [0x3fc, 0x400, 0x404, 0x408],
                                "stride_bytes": 16,
                                "maximum_nonclobbering_count": 19,
                                "source_clobber_iteration": 19,
                                "actual_input_count_validated": False}},
                        "tail_elf_virtual_address": 0x268e0,
                        "return_elf_virtual_address": 0x2696c,
                        "return_delay_slot_elf_virtual_address": 0x26970,
                        "tail_contains_calls_under_base_model": False,
                        "derived_word_stores": {
                            "base": "source value reloaded at 0x268c8",
                            "first_store_elf_virtual_address": 0x268ec,
                            "loop_store_elf_virtual_address": 0x26910,
                            "base_offset": 0x15128, "stride_bytes": 228,
                            "count": 34, "word_bytes": 4,
                            "first_context_offset_under_conditions": 0x158bc,
                            "last_context_offset_under_conditions": 0x17620},
                        "direct_context_word_stores": [
                            {"elf_virtual_address": 0x268e8, "context_offset": 0x33c},
                            {"elf_virtual_address": 0x2692c, "context_offset": 0x524},
                            {"elf_virtual_address": 0x26958, "context_offset": 0x528},
                            {"elf_virtual_address": 0x26960, "context_offset": 0x5b4}],
                        "largest_computed_context_offset_under_conditions": 0x176c4,
                        "maximum_aligned_context_base_without_wrap": 0xfffe8938,
                        "equal_value_on_selected_return_under_conditions": True,
                        "required_conditions": [
                            "GNU base-case instructions, delay slots and loop semantics; selected callees preserve the assumed registers",
                            "original call/literal edges survive unresolved relocation effects",
                            "unsigned constructor r14 count is at most 19; initialized source is unchanged through all earlier stores",
                            "four-byte-aligned context base is at most 0xfffe8938; all accessed context, derived and stack memory is valid, injectively mapped and non-aliasing",
                            "no concurrent or external changes to the tracked slots or reloaded source"],
                        "allocation_extent_validated": False,
                        "runtime_constructor_execution_validated": False,
                        "unconditional_constructor_exit_identity_validated": False},
                    "channel_table": {
                        "constructor_channel_load_elf_virtual_address": 0x2677c,
                        "constructor_table_literal": 0x3fffd470,
                        "constructor_table_offset": -248,
                        "constructor_context_store_elf_virtual_address": 0x26798,
                        "activation_channel_shift_elf_virtual_address": 0x9fa8,
                        "activation_table_literal": 0x3fffd368,
                        "activation_table_offset": 16,
                        "activation_context_load_elf_virtual_address": 0x9fbc,
                        "context_entry_local_base": 0x3fffd378, "channel_stride_bytes": 32,
                        "channel_range_validated": False, "active_channel_match_validated": False,
                        "entry_unchanged_validated": False},
                    "selected_snapshot_copy": {
                        "call_elf_virtual_address": 0x9fd0,
                        "original_callee_elf_virtual_address": 0x9e74,
                        "destination_delay_slot_elf_virtual_address": 0x9fd4,
                        "local_base_literal": 0x3fffcd70, "local_base_offset": 60,
                        "local_destination": 0x3fffcdac, "bytes": 0x5bc,
                        "word_bytes": 4, "chunk_bytes": 128, "full_chunks": 11, "tail_bytes": 60,
                        "fields": [
                            {"role": "producer_record_base", "source_context_offset": 0x21c,
                             "destination_local_address": 0x3fffcfc8},
                            {"role": "reader_completion_record_base", "source_context_offset": 0x530,
                             "destination_local_address": 0x3fffd2dc}],
                        "original_dma_read_callee_elf_virtual_address": 0x53c8,
                        "dma_read_call_elf_virtual_address": 0x9efc,
                        "dma_read_length_delay_slot_elf_virtual_address": 0x9f00,
                        "original_sync_callee_elf_virtual_address": 0x5364,
                        "sync_call_elf_virtual_addresses": [0x9eec, 0x9f48],
                        "original_local_copy_callee_elf_virtual_address": 0x52e0,
                        "local_copy_call_elf_virtual_addresses": [0x9f18, 0x9f5c],
                        "local_copy_length_delay_slot_elf_virtual_addresses": [0x9f1c, 0x9f60],
                        "successful_copy_validated": False},
                    "propagation": "equal local values only if the same channel snapshot successfully copies unchanged equal source slots",
                    "required_conditions": [
                        "GNU base-case ISA, selected callee semantics and register preservation",
                        "original call/literal edges survive the unresolved relocation/base model",
                        "same valid channel and unchanged matching context table entry",
                        "source slots remain equal after 0x268dc, including subsequent derived-pointer writes",
                        "no address overflow, source aliasing or concurrent changes invalidate the selected snapshot",
                        "successful initialized copy with the required DMA visibility and completion"],
                    "later_source_slot_equality_validated": False,
                    "runtime_snapshot_identity_validated": False},
                "outer_record_write": {"call_elf_virtual_address": 0xa7a8,
                                       "original_callee_elf_virtual_address": 0x537c,
                                       "delay_slot_elf_virtual_address": 0xa7ac, "bytes": 56},
                "outer_record_read": {"call_elf_virtual_address": 0xad00,
                                      "original_callee_elf_virtual_address": 0x53c8,
                                      "delay_slot_elf_virtual_address": 0xad04,
                                      "sync_call_elf_virtual_address": 0xad08,
                                      "bytes": 56, "scratch_local_address": 0x30051a80},
                "pointer_result_mailbox": {"arc_literal_base": 0x30000f00, "slot_offsets": [0, 4],
                                           "pointer_publication_mask": 0x80000000,
                                           "pointer_consumption_mask": 0x7fffffff},
                "inner_shared_prefix": {"call_elf_virtual_address": 0x2840,
                                        "original_callee_elf_virtual_address": 0x2160,
                                        "bytes": 48, "local_destination": 0x3fffc014},
                "selected_mpeg_packet_P": {"conditional_call_elf_virtual_address": 0x2958,
                                           "original_callee_elf_virtual_address": 0x441e4,
                                           "copy_call_elf_virtual_address": 0x44200,
                                           "original_copy_callee_elf_virtual_address": 0x2160,
                                           "copy_delay_slot_elf_virtual_address": 0x44204,
                                           "copy_bytes_under_base_model": 256,
                                           "local_destination": 0x3fffc2f0},
                "completion_boundary": {"record_F_offset": 32, "scratch_read_elf_virtual_address": 0x99d8,
                                        "store_elf_virtual_address": 0x99e4,
                                        "allocator_boundary_local_address": 0x3fffd31c,
                                        "meaning": "consumed boundary/bookkeeping only; not free or quiescence"}},
            "mpeg_argument_return_byte": {
                "parser_state_local_base": 0x3fffc000, "parser_byte_offset": 124,
                "original_helper_elf_virtual_address": 0x4374,
                "call_elf_virtual_address": 0x2d4dc, "argument_delay_slot_elf_virtual_address": 0x2d4e0,
                "argument_value": 3, "byte_store_elf_virtual_address": 0x2d4e4,
                "copy_source_state_offset": 120, "copy_bytes": 16, "packet_P_destination_offset": 180,
                "packet_P_byte_offset": 184, "copy_call_elf_virtual_address": 0x2ea44,
                "copy_delay_slot_elf_virtual_address": 0x2ea48,
                "inner_packet_local_base": 0x3fffc2f0, "inner_packet_word_local_address": 0x3fffc3a8,
                "inner_word_load_elf_virtual_address": 0x44154,
                "inner_state_local_base": 0x3fffc200, "inner_state_word_offset": -56,
                "inner_word_store_elf_virtual_address": 0x44164,
                "inner_byte_load_elf_virtual_address": 0x405ec,
                "comparison_values": [2, 1, 3],
                "comparison_elf_virtual_addresses": [0x40610, 0x40640, 0x40664],
                "conditional_branch_elf_virtual_addresses": [0x40618, 0x40644, 0x40668],
                "original_branch_target_elf_virtual_addresses": [0x40640, 0x40664, 0x40674]}},
        "validation_scope": {"full_descriptor_validated": False, "worklist_validated": False,
                             "reference_lifetime_validated": False, "vendor_ISA_validated": False,
                             "native_backend_api_validated": False, "public_PPB_equivalence_validated": False,
                             "relocation_effects_validated": False, "inner_entry_selection_validated": False,
                             "record_base_identity_validated": False, "active_context_identity_validated": False,
                             "free_or_quiescence_validated": False, "bitstream_helper_semantics_validated": False},
        "validated_regions": validated, "instruction_windows": windows,
        "limitations": [
            "Original call/literal targets are conditional: selected relocation effects and runtime operands are not validated.",
            "Field paths assume the GNU base-case ISA interpretation, selected callees and register preservation conventions.",
            "Producer CORE+88 and reader/completion local 0x3fffd2dc are distinct record-base fields; constructor slot assignment and conditional snapshot offsets do not validate their current equality or active codec contexts.",
            "The immediate assignment alone is not constructor exit identity; the separate return-edge lemma requires the bounded input count, unchanged source, valid non-aliasing memory and no address overflow.",
            "Mailbox transfer is not a direct cross-image function call or a host-callable native backend ABI.",
            "The computed STATUS dispatcher is not validated; the selected MPEG call is conditional on reaching that path.",
            "Overlay/BSS initialization, DMA coherence and runtime execution are not observed.",
            "SiU argument/return linkage does not establish bitstream parsing semantics or name comparison values."]}


def _command_buffer_bridge_map(payload, images):
    """Fixed initialized-path proof; no execution or general ARC relocation API."""
    identities = [(0x2ea60, 0x79dd8, 55), (0x79dd8, 0xcfbb0, 112)]
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            [(i.get("blob_file_offset"), i.get("blob_file_end"), i.get("section_count"))
             for i in images] != identities or
            any((i.get("class"), i.get("endianness"), i.get("machine"),
                 i.get("elf_type"), i.get("flags")) != (32, "little", 45, 2, 0)
                for i in images)):
        raise FormatError("command-buffer bridge image identities do not match the baseline")
    regions = _COMMAND_BUFFER_BRIDGE_REGIONS
    relocation_offset, relocation_size = 0x72780, 0x65c4
    total = sum(len(expected) // 2 for _, _, expected in regions) + relocation_size
    if (len(regions) + 1 > MAX_COMMAND_BUFFER_BRIDGE_REGIONS or
            total > MAX_COMMAND_BUFFER_BRIDGE_BYTES or
            relocation_size // 12 > MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS):
        raise FormatError("command-buffer bridge validation budget exceeded")
    validated = []
    # Pin every control/data window and the complete selected relocation table
    # before decoding branches, ELF fields, symbols or relocation arithmetic.
    for name, offset, expected in regions:
        expected = bytes.fromhex(expected)
        actual = bounded(payload, offset, len(expected), "command-buffer bridge region")
        if actual != expected:
            raise FormatError(f"command-buffer bridge region {name} does not match the baseline")
        validated.append({"role": name, "blob_file_offset": offset, "size": len(actual),
                          "sha256": hashlib.sha256(actual).hexdigest()})
    relocation_data = bounded(payload, relocation_offset, relocation_size,
                              "command-buffer bridge relocation table")
    if hashlib.sha256(relocation_data).hexdigest() != _COMMAND_BUFFER_BRIDGE_RELA_SHA256:
        raise FormatError("command-buffer bridge relocation table does not match the baseline")
    validated.append({"role": "outer_text_relocations", "blob_file_offset": relocation_offset,
                      "size": relocation_size, "sha256": _COMMAND_BUFFER_BRIDGE_RELA_SHA256})

    anchors = []
    for offset, target, link, condition in (
            (0x5fbc, 0x60c8, False, 0), (0x60d4, 0x5ccc, True, 14),
            (0x5cf8, 0x5d2c, False, 0), (0x5d38, 0x7534, True, 14),
            (0x5d4c, 0x54c, True, 14), (0x57c, 0x1dc, True, 14),
            (0x4a0, 0x200a4, True, 14), (0x4f4, 0x200d8, True, 14),
            (0x200c4, 0x2c624, True, 14), (0x2017c, 0x294ec, True, 14),
            (0x63c, 0xe87c, True, 14), (0xe890, 0x20708, True, 14),
            (0x7e0, 0xe8b0, True, 14), (0xe930, 0x20708, True, 14),
            (0xebb8, 0x26dd8, True, 14), (0xec30, 0x28310, True, 14),
            (0xec54, 0x26658, True, 14), (0x283a0, 0x1f5d4, True, 14),
            (0x283c8, 0x1fe6c, True, 14), (0x266a4, 0x26358, True, 14),
            (0x26374, 0x28050, True, 14), (0x28088, 0x27dc0, True, 14),
            (0x280b4, 0x27bd0, True, 14), (0x28120, 0x27bd0, True, 14),
            (0x27d74, 0x2af2c, True, 14), (0x2afd8, 0x2a568, True, 14),
            (0x2b010, 0x2a474, True, 14), (0x2b018, 0x2a3e0, True, 14),
            (0x2b0cc, 0x2a35c, True, 14), (0x2b16c, 0x29ff4, True, 14),
            (0x2a178, 0x29ba4, True, 14), (0x29c20, 0x29c98, False, 14),
            (0x29cc0, 0x29f00, False, 14), (0x2b21c, 0x29ae4, True, 14),
            (0x29b04, 0x1fdac, True, 14), (0x28178, 0x1fdac, True, 14),
            (0x288b0, 0x1fa08, True, 14), (0x270b8, 0x20708, True, 14),
            (0x29c28, 0x29d50, False, 14), (0x29d64, 0x29d84, False, 0),
            (0x29d94, 0x29db4, False, 0), (0x29dfc, 0x29f00, False, 14)):
        record = _a32_branch(payload, offset, link, condition)
        if record["target_blob_file_offset"] != target:
            raise FormatError("command-buffer bridge branch target does not match the baseline")
        anchors.append(record)
    for offset, position, value, register in (
            (0x5f78, 0x6174, 0x73763108, 2), (0x5d34, 0x5ea4, 0xd4164, 0),
            (0x5d44, 0x5ed4, 0xd1ff8, 1), (0x56c, 0x6fc, 0xd3a00, 4),
            (0x753c, 0x76b4, 0x03ffc000, 7), (0x7540, 0x76b8, 0x00116004, 5),
            (0x200bc, 0x20248, 0x2e21c, 1), (0xe888, 0xf6a4, 0x2dd04, 1),
            (0xe894, 0xf6a8, 0xcfcf0, 0), (0xe89c, 0xf6ac, 0xcfc00, 0)):
        record = _a32_literal(payload, offset)
        if (record["literal_blob_file_offset"], record["literal_value"],
                record["destination_register"]) != (position, value, register):
            raise FormatError("command-buffer bridge literal does not match the baseline")
        anchors.append(record)

    def section(table, index):
        return struct.unpack("<10I", bounded(payload, table + index * 40, 40,
                                              "command-buffer bridge section"))

    placements = []
    for slot, (table, limit, base_offset, image_start) in enumerate(
            ((0x79540, 17, 0, 0x2ea60), (0xcea30, 50, 0x90000, 0x79dd8))):
        eligible = [index for index in range(1, limit + 1)
                    if (lambda s: s[1] == 1 and s[2] & 2 and not s[2] & 4 and
                        s[5] and s[3] < 0x30000000)(section(table, index))]
        if eligible != [limit]:
            raise FormatError("command-buffer bridge first data section does not match the baseline")
        first = section(table, limit)
        placements.append({"slot": slot, "image_blob_file_offset": image_start,
                           "image_base_offset_from_B": base_offset,
                           "first_data_section_index": limit,
                           "first_data_section_name": ".vdec_cmd_block" if slot == 0 else ".rodata",
                           "first_data_section_header_blob_file_offset": table + limit * 40,
                           "first_data_elf_virtual_address": first[3], "first_data_bytes": first[5],
                           "first_data_blob_file_offset": image_start + first[4],
                           "first_data_offset_from_B": base_offset + first[3]})
    symbol = struct.unpack("<IIIBBH", bounded(payload, 0x69c90, 16, "packet section symbol"))
    rela = struct.unpack("<IIi", bounded(payload, 0x72f00, 12, "packet pointer relocation"))
    source = section(0x79540, 16)
    target = section(0x79540, symbol[5])
    record_index = (0x72f00 - relocation_offset) // 12
    if (symbol != (0, 0x70000, 0x100, 3, 0, 17) or rela != (0x25830, 0x1204, 0x100) or
            struct.unpack_from("<IIi", relocation_data, record_index * 12) != rela or
            section(0x79540, 51)[1:2] != (4,) or section(0x79540, 51)[6:8] != (35, 16)):
        raise FormatError("command-buffer bridge symbol/RELA identity does not match the baseline")
    site = 0x2ea60 + source[4] + rela[0] - source[3]
    rebased_symbol_offset = placements[0]["first_data_offset_from_B"] + symbol[1] - target[3]
    patched_literal_offset = rebased_symbol_offset + rela[2]
    # The selected literal has one relocation; the adjacent load, arithmetic
    # and argument-move words have none. Other vendor semantics remain outside
    # scope. This is a fixed, budgeted table check, not a relocation interpreter.
    matches = 0
    for index in range(relocation_size // 12):
        record = struct.unpack_from("<IIi", relocation_data, index * 12)
        if 0x2582c <= record[0] < 0x25840:
            if record != rela:
                raise FormatError("command-buffer bridge ARC operand sites are unexpectedly relocated")
            matches += 1
    if matches != 1:
        raise FormatError("command-buffer bridge literal relocation is not unique")
    calls = []
    symbol_table = section(0x79540, 35)
    # These two fixed records only. Executable S/P stay normalized during
    # relocation; the same B is added when both code sections are copied.
    for name, position, symbol_position, expected in (
            ("Dma_Read", 0x72f0c, 0x6c3d0, (0x25840, 0x28606, 0)),
            ("Dma_Sync", 0x72f18, 0x6c3b0, (0x25848, 0x28406, 0))):
        record = struct.unpack("<IIi", bounded(payload, position, 12, "outer call relocation"))
        index = record[1] >> 8
        definition = struct.unpack("<IIIBBH", bounded(payload, symbol_position, 16, "outer call symbol"))
        destination = section(0x79540, definition[5])
        if (record != expected or 0x2ea60 + symbol_table[4] + index * 16 != symbol_position or
                definition[5] != 2 or not destination[2] & 4 or not source[2] & 4):
            raise FormatError("command-buffer bridge fixed call identity does not match the baseline")
        normalized_source = source[3] + record[0] - source[3]
        normalized_symbol = destination[3] + definition[1] - destination[3]
        instruction_position = 0x2ea60 + source[4] + record[0] - source[3]
        original = _bootstrap_word(payload, instruction_position)
        delta = normalized_symbol + record[2] - normalized_source - 4
        # Firmware diagnostics fall through to patching if their callees
        # return. These fixed records satisfy the checks; this offline assertion
        # is not a general rejecting policy of the firmware relocator.
        if delta & 3 or not -(1 << 21) <= delta < (1 << 21):
            raise FormatError("command-buffer bridge fixed call displacement is invalid")
        preserve_mask, displacement_mask = 0xf800007f, 0x07ffff80
        patched = (original & preserve_mask) | ((delta << 5) & displacement_mask)
        displacement_words = (patched >> 7) & 0xfffff
        if displacement_words & (1 << 19):
            displacement_words -= 1 << 20
        decoded_target = normalized_source + 4 + displacement_words * 4
        if patched != original or decoded_target != normalized_symbol + record[2]:
            raise FormatError("command-buffer bridge fixed call is not preserved")
        calls.append({"target": name, "vendor_type": record[1] & 255,
                      "relocation_record_blob_file_offset": position,
                      "symbol_index": index, "symbol_record_blob_file_offset": symbol_position,
                      "target_section_index": definition[5], "addend": record[2],
                      "instruction_blob_file_offset": instruction_position,
                      "normalized_source_address": normalized_source,
                      "normalized_symbol_address": normalized_symbol,
                      "normalization_excludes_B": True, "signed_byte_displacement": delta,
                      "signed_displacement_bits": 22, "four_byte_aligned": True,
                      "firmware_diagnostics": {"alignment_check_satisfied": True,
                                               "signed22_range_check_satisfied": True,
                                               "invalid_checks_log_then_fall_through": True,
                                               "fallthrough_requires_logging_callees_to_return": True,
                                               "diagnostic_path_taken_for_fixed_record": False},
                      "pc_bias_bytes": 4, "displacement_field_bits": 20,
                      "displacement_scale_bytes": 4, "preserved_mask": preserve_mask,
                      "displacement_mask": displacement_mask,
                      "original_word": original, "patched_word": patched,
                      "original_bytes_hex": struct.pack("<I", original).hex(),
                      "patched_bytes_hex": struct.pack("<I", patched).hex(),
                      "instruction_bytes_unchanged": True, "write_byte_order": "little",
                      "byte_store_blob_file_offsets": [0x29dd8, 0x29de0, 0x29dec, 0x29df8],
                      "loaded_source_offset_from_B": normalized_source,
                      "loaded_target_offset_from_B": decoded_target,
                      "loaded_target_equation": "(B + P) + 4 + displacement = B + S + A",
                      "runtime_call_observed": False})
    heap_start, heap_end = _bootstrap_word(payload, 0x76b8), _bootstrap_word(payload, 0x76b4)
    alignment = max(_bootstrap_word(payload, 0x2e21c), 2)
    mask = (1 << alignment) - 1
    admitted_low = ((heap_start + 3) & ~3) + 100
    admitted_low = (admitted_low + mask) & ~mask
    admitted_high = heap_end & ~mask
    return {
        "device_observed": False, "base_symbol": "B",
        "scope": "Static initialized outer command-buffer address bridge, not standalone operation or codec acceptance.",
        "validated_regions": validated, "validated_byte_count": total,
        "relocation_record_count": relocation_size // 12, "instruction_anchors": anchors,
        "host_init": {"command": 0x73763001, "requires_uninitialized_host_context": True,
                      "handler_entry_blob_file_offset": 0x5ccc,
                      "factory_call_blob_file_offset": 0x7e0, "configuration_copy_bytes": 72,
                      "callback_context_offset": 0x3c, "catalog_context_offset": 0x40,
                      "callback_table_blob_file_offset": 0xcfcf0,
                      "catalog_blob_file_offset": 0xcfbe8, "combined_image_slot": 4,
                      "combined_image_slot_is_null": True, "fallback_image_slots": [0, 1]},
        "initialized_heap": {"virtual_base": heap_start, "physical_base": heap_start,
                             "bytes": heap_end - heap_start, "alignment_exponent": alignment,
                             "inclusive_virtual_range": [admitted_low, admitted_high],
                             "map_offsets": {"virtual_base": 0x28, "physical_base": 0x30,
                                             "inclusive_low": 0x18, "inclusive_high": 0x1c},
                             "translation": "virtual_base + physical_input - physical_base",
                             "initialized_translation_is_identity": True,
                             "constructor_blob_file_offset": 0x294ec,
                             "translation_entry_blob_file_offset": 0x1fdac},
        "allocation": {"bytes": 0x100000, "alignment_exponent": 12,
                       "base_definition": "B is the translated allocated block base in context+0x1b0.",
                       "virtual_pointer_context_offset": 0x1ac, "physical_pointer_context_offset": 0x1b0,
                       "owned_flag_byte_context_offset": 0x1b8, "owned_flag_value": 1,
                       "allocate_call_blob_file_offset": 0x283a0,
                       "release_call_blob_file_offset": 0x288b0,
                       "packet_is_subrange_not_separate_allocation": True},
        "image_placements": placements,
        "arm_packet": {"offset_from_B": rebased_symbol_offset, "bytes": target[5],
                       "physical_pointer_context_offset": 0x1cc,
                       "virtual_pointer_context_offsets": [0x94, 0x98],
                       "physical_store_blob_file_offset": 0x280f4,
                       "virtual_store_blob_file_offsets": [0x28180, 0x28188],
                       "copy_call_blob_file_offset": 0x270b8, "copy_bytes": 252},
        "outer_pointer_relocation": {"relocation_record_blob_file_offset": 0x72f00,
                                     "relocation_section_index": 51, "relocation_record_index": record_index,
                                     "vendor_type": rela[1] & 255, "symbol_index": rela[1] >> 8,
                                     "symbol_record_blob_file_offset": 0x69c90,
                                     "symbol_section_index": symbol[5], "original_symbol_value": symbol[1],
                                     "addend": rela[2], "symbol_offset_from_B": rebased_symbol_offset,
                                     "literal_blob_file_offset": site,
                                     "literal_elf_virtual_address": rela[0], "original_literal": 0x70100,
                                     "selected_literal_relocation_is_unique": True,
                                     "patched_literal_offset_from_B": patched_literal_offset,
                                     "semantics": "This pinned ARM type-4 handler writes 32-bit little-endian S+A.",
                                     "byte_store_blob_file_offsets": [0x29c9c, 0x29ca4, 0x29cb0, 0x29cbc],
                                     "applied_before_section_copy": True,
                                     "apply_call_blob_file_offset": 0x2b16c,
                                     "copy_call_blob_file_offset": 0x2b21c},
        "outer_arc_operand": {"function": "Core_Command", "image_slot": 0,
                              "function_elf_virtual_address": 0x25808,
                              "function_blob_file_offset": 0x4839c,
                              "load_instruction_blob_file_offset": 0x483c0,
                              "add_negative_256_blob_file_offset": 0x483c8,
                              "dma_argument_move_blob_file_offset": 0x483cc,
                              "dma_call_blob_file_offset": 0x483d4,
                              "dma_function": "Dma_Read", "dma_function_blob_file_offset": 0x3026c,
                              "dma_operand_offset_from_B": patched_literal_offset - 0x100,
                              "matches_arm_packet": patched_literal_offset - 0x100 == rebased_symbol_offset},
        "outer_call_relocations": calls,
        "validation_scope": {"selected_type4_literals": 1, "selected_type6_calls": 2,
                             "all_other_relocations_validated": False,
                             "vendor_extensions_validated": False,
                             "host_memory_access_validated": False, "operational_dma_observed": False},
        "assumptions": [
            "Fresh INIT reaches these paths; allocations, helper initialization and both ELF loads succeed.",
            "Copy, allocator, logging and other callees obey their observed calling convention and preserve required context/map fields.",
            "Selected standard ARC operand annotations agree with legacy decoding; the complete vendor ISA and extension semantics are unvalidated.",
            "Operational ARC DMA completion is not established by preserving the selected branch instructions."],
        "limitations": [
            "Only the selected type-4 literal and two type-6 call relocations are validated; other vendor relocations remain unvalidated.",
            "B is dynamic and has not been read from a running device; this is not a memory-access or ownership-borrowing API.",
            "The bridge is for the outer command buffer, not an inner decoder packet or a complete instruction call graph.",
            "Initialized range checks do not establish quiescence, safe raw-register access, runtime acceptance or silicon capability."]}


def _fresh_init_arm_operand(payload, offset, expected):
    """Narrow fixed A32 operands, not execution or a general decoder."""
    word = _bootstrap_word(payload, offset)
    if word != expected:
        raise FormatError("fresh INIT ARM operand does not match the baseline")
    if (word >> 25) & 7 == 5:
        return _a32_branch(payload, offset, bool(word & (1 << 24)), word >> 28)
    if word & 0xff7f0000 == 0xe51f0000:
        return _a32_literal(payload, offset)
    record = {"blob_file_offset": offset, "word": word, "condition": word >> 28}
    if word & 0xfffffff0 == 0xe12fff30:
        record.update(operation="BLX register", operand_register=word & 15)
    elif word & 0x0ff00000 == 0x03400000:
        raise FormatError("fresh INIT operand does not support MOVT")
    elif word & 0x0ff00000 == 0x03000000:
        record.update(operation="MOVW", destination_register=(word >> 12) & 15,
                      immediate=((word >> 4) & 0xf000) | (word & 0xfff))
    elif (word >> 26) & 3 == 1:
        if word & (1 << 21) or not word & (1 << 24):
            raise FormatError("fresh INIT memory operand has unsupported writeback")
        record.update(operation="LDR" if word & (1 << 20) else "STR",
                      base_register=(word >> 16) & 15, data_register=(word >> 12) & 15,
                      byte_width=1 if word & (1 << 22) else 4)
        if word & (1 << 25):
            if word & 0x70 or not word & (1 << 23):
                raise FormatError("fresh INIT memory shift is unsupported")
            record.update(offset_register=word & 15, shift_kind="LSL", shift_amount=(word >> 7) & 31)
        else:
            record["byte_offset"] = (word & 0xfff) * (1 if word & (1 << 23) else -1)
    elif (word >> 26) & 3 == 0:
        opcode = (word >> 21) & 15
        if opcode not in (4, 10, 13, 15) or (not word & (1 << 25) and word & 0x70):
            raise FormatError("fresh INIT data operand is unsupported")
        record.update(operation={4: "ADD", 10: "CMP", 13: "MOV", 15: "MVN"}[opcode],
                      source_register=(word >> 16) & 15,
                      destination_register=(word >> 12) & 15)
        if word & (1 << 25):
            value, shift = word & 255, ((word >> 8) & 15) * 2
            record["immediate"] = ((value >> shift) | (value << ((32 - shift) % 32))) & 0xffffffff
        else:
            record["operand_register"] = word & 15
            record.update(shift_kind="LSL", shift_amount=(word >> 7) & 31)
    else:
        raise FormatError("fresh INIT operand is outside the fixed decoder")
    return record


def _fresh_init_causal_contract(payload, images):
    """Private, conditional fresh stock INIT chain; never a runtime/lease API."""
    regions = _FRESH_INIT_REGIONS
    additional = sum(size for _, _, size, _ in regions)
    bridge_bytes = sum(len(expected) // 2 for _, _, expected in _COMMAND_BUFFER_BRIDGE_REGIONS) + 0x65c4
    anchor_count = len(_FRESH_INIT_ARM_SITES) + len(_FRESH_INIT_ARC_SITES) + len(_FRESH_INIT_ARC_CALLS)
    # All invoked dependency budgets are checked before any interpretation.
    # Overlapping pins intentionally count twice in this conservative receipt.
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            len(regions) > MAX_FRESH_INIT_REGIONS or additional > MAX_FRESH_INIT_BYTES or
            additional + bridge_bytes > MAX_FRESH_INIT_AGGREGATE_BYTES or
            anchor_count > MAX_FRESH_INIT_ANCHORS or MAX_FRESH_INIT_EVENTS < 35 or
            len(_COMMAND_BUFFER_BRIDGE_REGIONS) + 1 > MAX_COMMAND_BUFFER_BRIDGE_REGIONS or
            bridge_bytes > MAX_COMMAND_BUFFER_BRIDGE_BYTES or
            0x65c4 // 12 > MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS or
            MAX_STOCK_HOST_COMMAND_CFG_STATES < 53):
        raise FormatError("fresh INIT validation budget/identity exceeded")
    validated = []
    for name, offset, size, digest in regions:
        data = bounded(payload, offset, size, "fresh INIT region")
        if hashlib.sha256(data).hexdigest() != digest:
            raise FormatError(f"fresh INIT region {name} does not match the baseline")
        validated.append({"role": name, "blob_file_offset": offset, "size": size, "sha256": digest})
    # Pre-pin the entire invoked bridge before allowing either dependency to
    # interpret it. The dependency repeats its own unchanged bounded checks.
    for name, offset, expected in _COMMAND_BUFFER_BRIDGE_REGIONS:
        if bounded(payload, offset, len(expected) // 2, "fresh INIT bridge pin") != bytes.fromhex(expected):
            raise FormatError(f"fresh INIT bridge region {name} does not match the baseline")
    if hashlib.sha256(bounded(payload, 0x72780, 0x65c4, "fresh INIT bridge RELA pin")).hexdigest() != _COMMAND_BUFFER_BRIDGE_RELA_SHA256:
        raise FormatError("fresh INIT bridge relocation table does not match the baseline")
    bridge = _command_buffer_bridge_map(payload, images)
    # Explicitly one handler, not the 29-command selector or its 25-handler CFG.
    footprints = _stock_host_handler_footprints(payload, [0x5ccc])
    init_footprint = footprints[0]
    if (len(footprints) != 1 or init_footprint["entry_blob_file_offset"] != 0x5ccc or
            init_footprint["request_reads"] != [{"byte_offset": 4, "width": 4}] or
            [r["byte_offset"] for r in init_footprint["reply_writes"]] != [4, 8] or
            init_footprint["packet_header_reads"] or init_footprint["packet_header_writes"]):
        raise FormatError("fresh INIT-only stock receipt does not match")
    arm = {offset: _fresh_init_arm_operand(payload, offset, word)
           for offset, word in _FRESH_INIT_ARM_SITES}
    anchors = [dict(record, architecture="ARM") for record in arm.values()]

    def section(index):
        return struct.unpack("<10I", bounded(payload, 0x79540 + index * 40, 40, "fresh INIT section"))

    def position(index, address, size=4):
        s = section(index)
        if s[1] != 1 or not s[2] & 4 or not s[3] <= address <= s[3] + s[5] - size:
            raise FormatError("fresh INIT ARC site escaped its executable section")
        return 0x2ea60 + s[4] + address - s[3]

    symbol_table, string_table = section(35), section(34)
    if (symbol_table[1], symbol_table[6], symbol_table[9], string_table[1]) != (2, 34, 16, 3):
        raise FormatError("fresh INIT outer symbol tables do not match")
    symbols = {}
    for name, index, sec, address, size in (
            ("CmdInitialize", 46, 16, 0x245ec, 412), ("Core_Command", 554, 16, 0x25808, 516),
            ("Arc_FlushWrites", 556, 4, 0x8090, 24), ("Core_Loop", 623, 16, 0x2664c, 172),
            ("Core_LocalClear", 640, 2, 0x52bc, 36), ("Dma_Sync", 644, 2, 0x5364, 24),
            ("Dma_Write", 645, 2, 0x537c, 76), ("Dma_Read", 646, 2, 0x53c8, 68),
            ("Platform_EnableInterface", 800, 16, 0x3a960, 168),
            ("Platform_DeliverResponse", 801, 4, 0xbdc4, 40)):
        sympos = 0x2ea60 + symbol_table[4] + index * 16
        st_name, value, actual_size, info, other, actual_sec = struct.unpack(
            "<IIIBBH", bounded(payload, sympos, 16, "fresh INIT function symbol"))
        namepos = 0x2ea60 + string_table[4] + st_name
        if (value, actual_size, actual_sec, info & 15, other) != (address, size, sec, 2, 0) or bounded(
                payload, namepos, len(name) + 1, "fresh INIT symbol name") != name.encode() + b"\0":
            raise FormatError("fresh INIT function ownership/symbol does not match")
        bodypos = position(sec, address, size)
        if not any(low <= bodypos and bodypos + size <= low + count
                   for _, low, count, _ in regions) and not any(
                low <= bodypos and bodypos + size <= low + len(expected) // 2
                for _, low, expected in _COMMAND_BUFFER_BRIDGE_REGIONS):
            raise FormatError("fresh INIT selected function body is not fully pinned")
        symbols[name] = {"name": name, "symbol_index": index, "symbol_record_blob_file_offset": sympos,
                         "name_blob_file_offset": namepos, "section_index": sec,
                         "elf_virtual_address": value, "size": size, "blob_file_offset": bodypos}

    arc = {}
    for sec, address, expected in _FRESH_INIT_ARC_SITES:
        off = position(sec, address)
        word = _bootstrap_word(payload, off)
        if word != expected:
            raise FormatError("fresh INIT ARC operand does not match the baseline")
        # Standard legacy ARC fields only; the vendor ISA remains conditional.
        record = {"architecture": "ARC", "section_index": sec, "elf_virtual_address": address,
                  "blob_file_offset": off, "word": word, "decode_conditional": True,
                  "destination_register": (word >> 21) & 63,
                  "source_register": (word >> 15) & 63,
                  "operand_register": (word >> 9) & 63,
                  "low9": word & 511, "signed_low9": (word & 511) - (512 if word & 256 else 0)}
        if word >> 27 == 4:
            displacement = (word >> 7) & 0xfffff
            if displacement & (1 << 19):
                displacement -= 1 << 20
            record.update(operation="B", condition=word & 31,
                          target_elf_virtual_address=address + 4 + displacement * 4,
                          pc_bias_bytes=4, delay_slot_semantics="taken only" if word & 64 else
                          "always executed" if word & 32 else "none")
        if address in (0x26668, 0x25824, 0x2586c, 0x259dc, 0x3a984, 0xbdc4, 0xbdcc, 0xbdd4, 0xbddc):
            record["literal_value"] = _bootstrap_word(payload, position(sec, address + 4))
        arc[address] = record
        anchors.append(record)
    relas = [struct.unpack_from("<IIi", payload, 0x72780 + i * 12) for i in range(2171)]
    receipts = []
    for caller, address, callee, relapos in _FRESH_INIT_ARC_CALLS:
        src, dst = symbols[caller], symbols[callee]
        if not src["elf_virtual_address"] <= address <= src["elf_virtual_address"] + src["size"] - 4:
            raise FormatError("fresh INIT ARC call has wrong source function ownership")
        off = position(src["section_index"], address)
        original = _bootstrap_word(payload, off)
        preserve_mask, displacement_mask = 0xf800007f, 0x07ffff80
        if original & preserve_mask not in (0x28000000, 0x28000020):
            raise FormatError("fresh INIT selected ARC edge is not BL/BL.d")
        records = [(0x72780 + i * 12, r) for i, r in enumerate(relas) if r[0] == address]
        if relapos is None:
            if records or src["section_index"] != dst["section_index"]:
                raise FormatError("fresh INIT same-section resolved call has unexpected relocation")
            vendor_type, addend = None, 0
        else:
            expected = (address, (dst["symbol_index"] << 8) | 6, 0)
            if records != [(relapos, expected)]:
                raise FormatError("fresh INIT ARC call relocation is not uniquely section-owned")
            vendor_type, addend = 6, expected[2]
        delta = dst["elf_virtual_address"] + addend - address - 4
        if delta & 3 or not -(1 << 21) <= delta < (1 << 21):
            raise FormatError("fresh INIT ARC call displacement is invalid")
        patched = (original & preserve_mask) | ((delta << 5) & displacement_mask)
        encoded = (patched >> 7) & 0xfffff
        signed = encoded - (1 << 20) if encoded & (1 << 19) else encoded
        decoded = address + 4 + signed * 4
        if patched != original or decoded != dst["elf_virtual_address"] + addend:
            raise FormatError("fresh INIT ARC call is not preserved at the selected symbol")
        delay = bool(original & 32)
        receipt = {"architecture": "ARC", "caller": caller, "callee": callee,
                   "source_section_index": src["section_index"], "target_section_index": dst["section_index"],
                   "instruction_blob_file_offset": off, "source_elf_virtual_address": address,
                   "target_elf_virtual_address": dst["elf_virtual_address"], "symbol_index": dst["symbol_index"],
                   "symbol_record_blob_file_offset": dst["symbol_record_blob_file_offset"],
                   "relocation_record_blob_file_offset": relapos, "relocation_section_index": 51 if relapos else None,
                   "vendor_type": vendor_type, "addend": addend, "original_word": original, "patched_word": patched,
                   "instruction_bytes_unchanged": True, "pc_bias_bytes": 4, "signed_byte_displacement": delta,
                   "preserved_mask": preserve_mask, "displacement_mask": displacement_mask,
                   "decoded_target_elf_virtual_address": decoded, "normalization_excludes_B": True,
                   "delay_slot": delay, "delay_slot_elf_virtual_address": address + 4 if delay else None,
                   "delay_slot_word": _bootstrap_word(payload, position(src["section_index"], address + 4)) if delay else None,
                   "delay_slot_semantics": "always executed" if delay else "none",
                   "decode_conditional": True, "runtime_observed": False}
        receipts.append(receipt)
        anchors.append(receipt)

    def immediate(offset):
        return arm[offset]["immediate"]

    def displacement(offset):
        return arm[offset]["byte_offset"]

    def branch(offset, target, condition=0):
        if (arm[offset]["target_blob_file_offset"], arm[offset]["condition"]) != (target, condition):
            raise FormatError("fresh INIT selected predicate/branch does not match")

    # Bind the checked return chain to saved r0, CMP(saved,0), BEQ(success),
    # and MOV(r0,saved) on the error edge, rather than merely naming callees.
    chains = []
    for caller, call, save, compare, edge, success, error in (
            ("host", 0x5d4c, 0x5d50, 0x5d54, 0x5d58, 0x5f08, 0x5d74),
            ("context", 0x7e0, 0x7e4, 0x7e8, 0x7ec, 0x814, None),
            ("factory", 0xec54, 0xec58, 0xec5c, 0xec60, 0xec74, 0xec6c),
            ("image", 0x266a4, 0x266a8, 0x266ac, 0x266b0, 0x266bc, 0x266b4),
            ("loader", 0x263c8, 0x263cc, 0x263d0, 0x263d4, 0x263e8, 0x263e0),
            ("builder", 0x27254, 0x27258, 0x27288, 0x2728c, 0x273b4, 0x27298)):
        saved = arm[save]["destination_register"]
        if (arm[save].get("operation"), arm[save].get("operand_register"),
                arm[compare].get("operation"), arm[compare].get("source_register"),
                immediate(compare)) != ("MOV", 0, "CMP", saved, 0):
            raise FormatError("fresh INIT checked return operands are not coherent")
        branch(edge, success)
        if error is not None and (arm[error].get("operation"), arm[error].get("destination_register"),
                                  arm[error].get("operand_register")) != ("MOV", 0, saved):
            raise FormatError("fresh INIT error return does not preserve the saved result")
        chains.append({"caller": caller, "call_blob_file_offset": call, "saved_register": saved,
                       "save_blob_file_offset": save, "compare_blob_file_offset": compare,
                       "success_branch_blob_file_offset": edge, "success_blob_file_offset": success,
                       "error_return_blob_file_offset": error, "success_compare_value": immediate(compare),
                       "error_preserves_result": True,
                       "call_receipt_scope": "bridge" if call not in arm else "fresh_init"})
    if (arm[0x860].get("operand_register"), arm[0x860].get("destination_register"),
            arm[0x22dc8].get("operand_register"), arm[0x22dec].get("operand_register")) != (5, 3, 3, 4):
        raise FormatError("fresh INIT context logging return does not preserve the result")
    for offset, target, condition in ((0x27088, 0x27098, 0), (0x270ec, 0x27100, 1),
                                      (0x2711c, 0x271a0, 0), (0x2713c, 0x27158, 0),
                                      (0x27168, 0x271b0, 0)):
        branch(offset, target, condition)
    for offset, target in ((0x270a8, 0x20690), (0x270c8, 0x25024), (0x270e0, 0x20598),
                           (0x27110, 0x25010), (0x2712c, 0x20708), (0x2c188, 0x20614),
                           (0xeb88, 0x2052c), (0x28214, 0x6ea0), (0xf0, 0x6ef0),
                           (0x26e04, 0x2c624), (0x868, 0x22db8)):
        branch(offset, target, 14)
    if (arm[0x271c0].get("operand_register"), arm[0x270e4].get("destination_register"),
            arm[0x273f4].get("operand_register"), arm[0x27258].get("destination_register")) != (9, 9, 10, 10):
        raise FormatError("fresh INIT success return does not retain the original wait result")
    table_base = arm[0x26dfc]["literal_value"]
    table_context = immediate(0x26e00)
    table_size = immediate(0x26df8)
    if table_base != 0xcfc04 or table_size != 236:
        raise FormatError("fresh INIT register table copy does not match")
    table = {name: {"context_offset": offset,
                    "table_blob_file_offset": table_base + offset - table_context,
                    "value": _bootstrap_word(payload, table_base + offset - table_context)}
             for name, offset in (("init_response_target", displacement(0x27234)),
                                  ("generic_callback_selector", 0xcc),
                                  ("completion", displacement(0x27108)),
                                  ("publication", displacement(0x270bc)))}
    transport = {"entry_blob_file_offset": 0x2705c,
                 "busy_context_offset": displacement(0x27080),
                 "busy_status": arm[0x27090]["literal_value"],
                 "event_context_offset": displacement(0x270a4), "copy_bytes": immediate(0x270ac),
                 "packet_virtual_context_offset": displacement(0x270b4),
                 "packet_physical_context_offset": displacement(0x270c0),
                 "publication_register_context_offset": displacement(0x270bc),
                 "completion_register_context_offset": displacement(0x27108),
                 "timeout_argument": immediate(0x2723c), "timeout_status": immediate(0x270e8),
                 "zero_completion_status": immediate(0x271a8),
                 "command_mismatch_status": immediate(0x27150), "backend_error_status": immediate(0x27198),
                 "command_word_offset": displacement(0x27130),
                 "backend_status_word_offset": displacement(0x27160),
                 "success_returns_wait_status": True, "native_wait_statuses": [immediate(0x205a4), immediate(0x205e4)],
                 "zero_completion_preserves_busy": True, "locks_are_bare_returns": True,
                 "busy_acquire_value": immediate(0x27098),
                 "busy_free_value": immediate(0x27084),
                 "busy_acquire_offset": displacement(0x2709c),
                 "busy_clear_offsets": {"timeout": displacement(0x270f4), "command_mismatch": displacement(0x27144),
                                        "backend_status": displacement(0x27170), "success": displacement(0x271b4)},
                 "native_wait_success_consumes_event": arm[0x20608]["operation"] == "STR" and arm[0x20608]["byte_width"] == 1,
                 "event_consumption_offset": displacement(0x20608),
                 "predicates": {"busy_equals": immediate(0x27084), "timeout_equals": immediate(0x270e8),
                                "completion_not_equals": immediate(0x27118),
                                "command_compare_registers": [arm[0x27138]["source_register"], arm[0x27138]["operand_register"]],
                                "backend_status_equals": immediate(0x27164)},
                 "order": ["busy_check", "event_reset", "request_copy", "request_publication", "event_wait",
                           "completion_read", "reply_copy", "command_check", "backend_status_check", "success_reset"],
                 "operand_receipts": {hex(offset): arm[offset] for offset in arm if 0x2705c <= offset < 0x271c8}}
    enable_edge = next(r for r in receipts if r["callee"] == "Platform_EnableInterface")
    zero = arc[0x246d0]
    if (zero["destination_register"], zero["source_register"], zero["operand_register"],
            arc[0x246e0]["operand_register"]) != (2, 2, 2, 2):
        raise FormatError("fresh INIT backend status store is not the self-subtracted zero")
    outer = {"symbols": list(symbols.values()), "internal_command": arc[0x2586c]["literal_value"],
             "local_packet_address": arc[0x25824]["literal_value"],
             "local_mailbox_base": arc[0x259dc]["literal_value"],
             "trigger_offset": arc[0x266c4]["low9"], "trigger_bit_mask": arc[0x266c8]["low9"],
             "reply_mailbox_offset": arc[0x259e8]["low9"],
             "init_response_word_offset": enable_edge["delay_slot_word"] & 511,
             "reply_backend_status_offset": arc[0x246e0]["low9"],
             "reply_backend_status": zero["source_register"] - zero["operand_register"],
             "response_target_storage": arc[0xbdc4]["literal_value"],
             "response_address_mask": arc[0xbdcc]["literal_value"],
             "response_address_prefix": arc[0xbdd4]["literal_value"],
             "response_irq_bits": arc[0xbddc]["literal_value"],
             "operands": list(arc.values()),
             "transport_edges": [r["callee"] for r in receipts if r["caller"] == "Core_Command"],
             "ordinary_call_delay_slots_always_execute": True,
             "conditional_BZ_jd_slot_executes_only_when_taken": True,
             "dma_completion_and_visibility_assumed": True}
    if (outer["internal_command"] != arm[0x27210]["literal_value"] or
            outer["init_response_word_offset"] != displacement(0x27238) or
            outer["response_target_storage"] != arc[0x3a984]["literal_value"] - 256 or
            outer["reply_backend_status_offset"] != displacement(0x27160)):
        raise FormatError("fresh INIT ARM/outer argument or response linkage is incoherent")
    trigger_edge = arc[0x266cc]
    if (trigger_edge["condition"], trigger_edge["target_elf_virtual_address"],
            arc[0x266c8]["source_register"]) != (2, 0x266dc, arc[0x266c4]["destination_register"]):
        raise FormatError("fresh INIT bit-set trigger predicate is not connected")
    outer["trigger"] = {"base_address": arc[0x26668]["literal_value"],
                        "register_offset": arc[0x266c4]["low9"], "bit_mask": arc[0x266c8]["low9"],
                        "condition": trigger_edge["condition"],
                        "branch_target_elf_virtual_address": trigger_edge["target_elf_virtual_address"],
                        "call_elf_virtual_address": next(r["source_elf_virtual_address"] for r in receipts if r["caller"] == "Core_Loop"),
                        "pending_byte_alternative_validated": False}
    range_edge = arc[0x25878]
    selected_edge = arc[0x2588c]
    if (arc[0x25874]["source_register"], range_edge["condition"],
            arc[0x25880]["low9"], arc[0x25884]["operand_register"],
            selected_edge["target_elf_virtual_address"]) != (1, 13, 3, 1, 0x258b0):
        raise FormatError("fresh INIT selected command switch operands are incoherent")
    status_pc = 0x2587c + 4
    table_entry = status_pc + arc[0x25880]["low9"] * 4
    if table_entry != selected_edge["elf_virtual_address"]:
        raise FormatError("fresh INIT conditional STATUS-PC table route is incoherent")
    outer["init_dispatch"] = {"command_base": outer["internal_command"], "command_index": 0,
                              "maximum_index": arc[0x25874]["low9"], "range_condition": range_edge["condition"],
                              "default_target_elf_virtual_address": range_edge["target_elf_virtual_address"],
                              "status_auxiliary_register": arc[0x2587c]["low9"],
                              "status_pc_next_elf_virtual_address": status_pc,
                              "table_bias_words": arc[0x25880]["low9"], "index_register": arc[0x25884]["operand_register"],
                              "jump_register": arc[0x25888]["source_register"],
                              "selected_entry_elf_virtual_address": table_entry,
                              "selected_target_elf_virtual_address": selected_edge["target_elf_virtual_address"],
                              "status_pc_word_address_semantics_assumed": True}
    enable_branch = arc[0x3a9b0]
    if (arc[0x3a980]["destination_register"], arc[0x3a980]["source_register"],
            arc[0x3a9ac]["source_register"], enable_branch["condition"],
            enable_branch["target_elf_virtual_address"], enable_branch["delay_slot_semantics"],
            arc[0x3a9b4]["destination_register"], arc[0x3a9b8]["operand_register"]) != (
            15, 1, 15, 1, 0x3a9bc, "taken only", 15, 15):
        raise FormatError("fresh INIT nonzero interface argument is not preserved")
    storage = arc[0x3a984]["literal_value"] + arc[0x3a9b8]["signed_low9"]
    if storage != outer["response_target_storage"] or arc[0x3a9b4]["signed_low9"] != arc[0x3a9b8]["signed_low9"]:
        raise FormatError("fresh INIT interface/delivery storage does not match")
    outer["enable_interface"] = {"argument_register": arc[0x3a980]["source_register"],
                                 "saved_register": arc[0x3a980]["destination_register"],
                                 "argument_value": table["init_response_target"]["value"],
                                 "zero_branch_condition": enable_branch["condition"],
                                 "zero_branch_target_elf_virtual_address": enable_branch["target_elf_virtual_address"],
                                 "delay_slot_semantics": enable_branch["delay_slot_semantics"],
                                 "old_value_loaded_only_for_zero_argument": True,
                                 "store_signed_offset": arc[0x3a9b8]["signed_low9"],
                                 "storage_address": storage, "nonzero_argument_stored": table["init_response_target"]["value"] != 0,
                                 "first_argument_branch_validated": False}
    if (arm[0x263bc]["operation"], arm[0x263bc]["data_register"], arm[0x271d8]["operation"],
            arm[0x271d8]["operand_register"], arm[0x271d8]["destination_register"],
            arm[0x27218]["data_register"]) != ("LDR", 2, "MOV", 2, 9, 9):
        raise FormatError("fresh INIT request word1 does not come from the selected byte")
    # Prove the two independently computed slot addresses and data-register
    # aliases, not merely a callback name or a claimed table stride.
    if (arm[0x28210]["destination_register"], arm[0x28210]["operand_register"],
            arm[0x6eb8]["destination_register"], arm[0x6eb8]["source_register"], arm[0x6eb8]["operand_register"],
            arm[0x6ebc]["base_register"], arm[0x6ebc]["offset_register"], arm[0x6ebc]["data_register"],
            arm[0x6ec0]["source_register"], arm[0x6ec0]["operand_register"],
            arm[0x6ec4]["data_register"], arm[0x6ec8]["data_register"],
            arm[0x6f94]["source_register"], arm[0x6f94]["operand_register"],
            arm[0x6f98]["source_register"], arm[0x6f98]["operand_register"],
            arm[0x6fa8]["base_register"], arm[0x6fa8]["offset_register"], arm[0x6fa8]["data_register"],
            arm[0x6fac]["base_register"], arm[0x6fac]["data_register"], arm[0x6fb0]["operand_register"],
            arm[0x2c170]["operand_register"], arm[0x2c178]["operand_register"]) != (
            0, 3, 0, 0, 0, 4, 0, 1, 4, 0, 2, 3, 4, 4, 9, 8, 9, 8, 2, 6, 0, 2, 0, 4):
        raise FormatError("fresh INIT IRQ callback register aliases are incoherent")
    if (arm[0x6eb4]["literal_value"] != arm[0x6f20]["literal_value"] or
            arm[0x2820c]["literal_value"] != 0x2c16c or
            displacement(0x6ec4) != displacement(0x6fac) or
            displacement(0x6ec8) != displacement(0x6f9c) or
            arm[0x6fa0]["source_register"] != arm[0x6f9c]["data_register"] or immediate(0x6fa0) != 0):
        raise FormatError("fresh INIT IRQ callback slot/table identity does not match")
    branch(0x6fa4, 0x6fc8)
    shift = arm[0x6eb8]["shift_amount"]
    scale = 1 + (1 << shift)
    stride = scale * (1 << arm[0x6ebc]["shift_amount"])
    if (shift != arm[0x6f94]["shift_amount"] or arm[0x6ebc]["shift_amount"] != arm[0x6ec0]["shift_amount"] or
            arm[0x6ebc]["shift_amount"] != arm[0x6f98]["shift_amount"] or
            arm[0x6ebc]["shift_amount"] != arm[0x6fa8]["shift_amount"] or
            immediate(0x28208) + displacement(0x2c184) != transport["event_context_offset"]):
        raise FormatError("fresh INIT IRQ callback address/event arithmetic is incoherent")
    registration = {"callback_address": arm[0x2820c]["literal_value"],
                    "table_address": arm[0x6eb4]["literal_value"], "slot": immediate(0x28204),
                    "slot_stride": stride, "slot_address": arm[0x6eb4]["literal_value"] + immediate(0x28204) * stride,
                    "callback_word_offset": 0, "userdata_word_offset": displacement(0x6ec4),
                    "flag_word_offset": displacement(0x6ec8), "flag_value": immediate(0x28204),
                    "flag_nonzero_selects_direct_callback": True, "callback_load_register": arm[0x6fa8]["data_register"],
                    "callback_branch_register": arm[0x6fb0]["operand_register"],
                    "userdata_context_offset": immediate(0x28208),
                    "event_offset_from_userdata": displacement(0x2c184),
                    "event_context_offset": immediate(0x28208) + displacement(0x2c184),
                    "requires_irq_slot_pending_and_table_preserved": True}
    host = {"command": bridge["host_init"]["command"], "internal_command": arm[0x27210]["literal_value"],
            "handler_entry_blob_file_offset": 0x5ccc, "handler_count": 1, "stock_init_receipt": init_footprint,
            "full_stock_selector_invoked": False, "shortcut_global_address": arm[0x5ce8]["literal_value"],
            "shortcut_returns0_without_backend": True,
            "shortcut_branch_receipt": next(a for a in bridge["instruction_anchors"] if a["blob_file_offset"] == 0x5cf8),
            "shortcut_branch_receipt_scope": "independently bounded bridge",
            "failure_reply_status": (~immediate(0x5d64)) & 0xffffffff,
            "success_reply_status": immediate(0x5cec), "success_return": immediate(0x5d10),
            "sequence_request_offset": init_footprint["request_reads"][0]["byte_offset"],
            "sequence_reply_offset": init_footprint["reply_writes"][0]["byte_offset"],
            "status_reply_offset": displacement(0x5d68)}
    return {"basis": {"baseline_firmware_sha256": BUNDLED_SHA256, "payload_bytes": len(payload),
                      "selected_regions_validated": True, "entire_payload_rehashed": False,
                      "conditional": True, "device_observed": False, "public_route": False},
            "validation": {"additional_region_count": len(regions), "additional_byte_count": additional,
                           "aggregate_byte_count": additional + bridge_bytes,
                           "bridge_region_count": len(bridge["validated_regions"]), "bridge_byte_count": bridge_bytes,
                           "causal_anchor_count": len(anchors), "bridge_anchor_count": len(bridge["instruction_anchors"]),
                           "projected_event_cap": MAX_FRESH_INIT_EVENTS, "validated_regions": validated},
            "bridge": bridge, "host_init": host,
            "arm_builder": {"entry_blob_file_offset": 0x271c8, "internal_command": host["internal_command"],
                            "request_word1_offset": displacement(0x27218), "request_word1_register": arm[0x27218]["data_register"],
                            "request_word1_source_width": arm[0x263bc]["byte_width"],
                            "request_word1_source_context_offset": displacement(0x263bc),
                            "request_word1_native_max": (1 << (8 * arm[0x263bc]["byte_width"])) - 1,
                            "response_target_context_offset": displacement(0x27234),
                            "response_target_word_offset": displacement(0x27238), "timeout_argument": immediate(0x2723c),
                            "preserves_transport_status_on_both_return_edges": True},
            "transport": transport, "checked_return_chain": chains,
            "event_path": {"event_context_offset": transport["event_context_offset"],
                           "registration": registration,
                           "callback_entry_blob_file_offset": arm[0x2c188]["blob_file_offset"] - 28,
                           "callback_userdata_context_offset": immediate(0x28208),
                           "callback_event_offset_from_userdata": displacement(0x2c184),
                           "irq_slot": immediate(0x28204), "irq_vector_entry": arm[0x18]["literal_value"],
                           "irq_dispatch_entry": arm[0xf0]["target_blob_file_offset"],
                           "event_set_value": immediate(0x20618), "event_reset_value": immediate(0x20694),
                           "event_byte_width": arm[0x2061c]["byte_width"],
                           "event_generation_check": False, "register_table": table,
                           "arm_mmio_base": immediate(0x7580)},
            "outer_path": outer, "relocation_receipts": receipts, "instruction_anchors": anchors,
            "assumptions": [
                "Fresh non-null stock INIT with a serialized controller/packet transaction; other initialization, allocation, ELF load, cleanup and logging helpers return successfully or preserve the checked primary error.",
                "Opaque callees, including logging/yield and post-transport setup, obey the observed calling convention, preserve required saved registers/stack/context and do not mutate packet words through undisclosed aliases; not all setup helper return values are checked.",
                "Selected standard legacy ARC operands and delay semantics apply, including STATUS exposing the next PC in word-address units for the fixed computed INIT table; unselected vendor ISA/relocations remain conditional and are not validated by this receipt.",
                "ARM 0x10000000-base register offsets alias the selected ARC local mailboxes; request publication triggers the selected Core_Loop path and response delivery reaches enabled ARM private IRQ9. These hardware routing/namespace facts are assumptions, not source proof.",
                "ARC polling/DMA/flush complete and packet writes become visible in order to both processors; cache coherence, DMA visibility and atomic/serialized execution are assumptions.",
                "Normal fresh inference excludes a delayed old callback/completion after event reset; event byte and mailbox have no transaction generation check. The countermodel explicitly drops this freshness assumption."],
            "validation_scope": {"fresh_init_only": True, "full_stock_selector": False,
                                 "conditional_software_chain": True, "opaque_setup_return_values_all_checked": False,
                                 "transport_zero_requires_successful_post_transport_setup": True,
                                 "hardware_aliasing_proven": False,
                                 "runtime_transaction_acknowledged": False, "source_plane_lease": False,
                                 "active_decode_context": False, "standalone_execution": False,
                                 "whole_firmware_relocation_closure": False}}


def _fresh_init_projection(contract, scenario):
    """Finite test-only trace; timeout omissions do not prove backend nonexecution."""
    defaults = {"already_initialized": False, "busy": False, "wait_status": 0,
                "completion_mailbox": 1, "reply_command": contract["host_init"]["internal_command"],
                "reply_status": 0, "request_word1": 0, "event_origin": "current", "freshness_assumed": True}
    if type(scenario) is not dict or set(scenario) - set(defaults):
        raise FormatError("fresh INIT projection has unsupported scenario fields")
    values = dict(defaults, **scenario)
    for name, value in values.items():
        if name in ("already_initialized", "busy", "freshness_assumed"):
            if type(value) is not bool:
                raise FormatError("fresh INIT projection boolean is invalid")
        elif name == "event_origin":
            if value not in ("current", "delayed_old") or type(value) is not str:
                raise FormatError("fresh INIT projection event origin is invalid")
        elif type(value) is not int or not 0 <= value <= 0xffffffff:
            raise FormatError("fresh INIT projection scalar is not u32")
    if values["event_origin"] == "delayed_old" and values["freshness_assumed"]:
        raise FormatError("delayed old response contradicts the freshness assumption")
    if values["request_word1"] > contract["arm_builder"]["request_word1_native_max"]:
        raise FormatError("fresh INIT projection request word1 exceeds the native byte domain")
    t, h, outer = contract["transport"], contract["host_init"], contract["outer_path"]
    cap = min(MAX_FRESH_INIT_EVENTS, contract["validation"]["projected_event_cap"])
    if type(cap) is not int or cap < 1:
        raise FormatError("fresh INIT projection event budget exceeded")
    events = []

    def event(name, **fields):
        if len(events) >= cap:
            raise FormatError("fresh INIT projection event budget exceeded")
        events.append(dict(event=name, **fields))

    native = values["wait_status"] in t["native_wait_statuses"]
    event("host_init", command=h["command"])
    busy_after, acknowledged, status, consumed = values["busy"], False, None, False
    current_path, observation_relaxed = False, False
    if values["already_initialized"]:
        event("initialized_shortcut", return_value=h["success_return"])
        result = h["success_return"]
    else:
        event("fresh_initialization", opaque_helpers_assumed=True)
        event("busy_check", context_offset=t["busy_context_offset"], busy=values["busy"])
        if values["busy"]:
            status = t["busy_status"]
        else:
            busy_after = bool(t["busy_acquire_value"])
            event("event_reset", context_offset=t["event_context_offset"])
            event("request_copy", bytes=t["copy_bytes"], command=h["internal_command"], word1=values["request_word1"])
            event("request_publication", context_offset=t["publication_register_context_offset"])
            if values["event_origin"] == "delayed_old" and values["wait_status"] != t["timeout_status"]:
                event("delayed_old_callback", freshness_relaxed=True)
            elif values["wait_status"] != t["timeout_status"]:
                # Each call name/order comes from section-owned decoded edges.
                current_path = True
                event("outer_trigger", register_offset=outer["trigger_offset"], bit_mask=outer["trigger_bit_mask"])
                loop_edge = next(r for r in contract["relocation_receipts"] if r["caller"] == "Core_Loop")
                event("outer_call", function=loop_edge["callee"])
                for name in outer["transport_edges"]:
                    if name == "Platform_DeliverResponse":
                        event("reply_publication", offset=outer["reply_mailbox_offset"])
                    event("outer_call", function=name)
                    if name == "CmdInitialize":
                        for nested in contract["relocation_receipts"]:
                            if nested["caller"] == name:
                                event("outer_call", function=nested["callee"])
                        event("backend_status_store", byte_offset=outer["reply_backend_status_offset"], value=outer["reply_backend_status"])
                event("response_irq", bits=outer["response_irq_bits"])
                event("arm_callback", event_set_value=contract["event_path"]["event_set_value"])
                observation_relaxed = (values["completion_mailbox"] == t["predicates"]["completion_not_equals"] or
                                       values["reply_command"] != h["internal_command"] or
                                       values["reply_status"] != outer["reply_backend_status"])
                if observation_relaxed:
                    event("observation_coherence_relaxed", selected_outer_reply_preserved=False)
            event("event_wait", return_value=values["wait_status"], native_output=native)
            if values["wait_status"] == t["native_wait_statuses"][0]:
                consumed = t["native_wait_success_consumes_event"]
                event("event_consumed", byte_offset=t["event_consumption_offset"])
            if values["wait_status"] == t["predicates"]["timeout_equals"]:
                busy_after = bool(t["busy_free_value"])
                status = t["timeout_status"]
            else:
                event("completion_read", context_offset=t["completion_register_context_offset"], value=values["completion_mailbox"])
                if values["completion_mailbox"] == t["predicates"]["completion_not_equals"]:
                    status = t["zero_completion_status"]
                else:
                    reply_command = values["reply_command"] if values["event_origin"] == "current" else h["internal_command"]
                    reply_status = values["reply_status"] if values["event_origin"] == "current" else values["request_word1"]
                    event("reply_copy", bytes=t["copy_bytes"], command=reply_command, word1=reply_status)
                    event("command_check", command=reply_command)
                    if reply_command != h["internal_command"]:
                        busy_after = bool(t["busy_free_value"])
                        status = t["command_mismatch_status"]
                    else:
                        event("backend_status_check", status=reply_status)
                        if reply_status != t["predicates"]["backend_status_equals"]:
                            busy_after = bool(t["busy_free_value"])
                            status = t["backend_error_status"]
                        else:
                            busy_after = bool(t["busy_free_value"])
                            event("success_reset", context_offset=t["event_context_offset"])
                            status = values["wait_status"]
                            acknowledged = current_path and not observation_relaxed and native
        result = status
        for stage in reversed(contract["checked_return_chain"]):
            event("checked_return", caller=stage["caller"], value=result,
                  success=result == stage["success_compare_value"])
    reply = h["success_reply_status"] if result == h["success_return"] else h["failure_reply_status"]
    event("host_reply_status_store", byte_offset=h["status_reply_offset"], value=reply)
    return {"events": events, "transport_status": status, "host_return": result, "host_reply_status": reply,
            "busy_after": busy_after, "busy_after_scope": "transport_return_boundary",
            "post_transport_opaque_setup_success_assumed": True,
            "projected_current_path": current_path, "observation_coherence_relaxed": observation_relaxed,
            "event_consumed": consumed, "current_transaction_acknowledged": acknowledged,
            "timeout_proves_backend_nonexecution": False,
            "freshness_assumed": values["freshness_assumed"], "native_wait_output": native,
            "opaque_wait_output_relaxed": not native, "runtime_observed": False}


def _init_reply_arm_operand(payload, offset, expected):
    """Additional fixed loader/translation operands; not a general decoder."""
    word = _bootstrap_word(payload, offset)
    if word != expected:
        raise FormatError("INIT reply ARM operand does not match the baseline")
    record = {"blob_file_offset": offset, "word": word, "condition": word >> 28}
    if (word >> 25) & 7 == 5 and word >> 28 in (3, 8, 9):
        if word & (1 << 24):
            raise FormatError("INIT reply unsigned branch does not support link")
        displacement = word & 0xffffff
        if displacement & 0x800000:
            displacement -= 1 << 24
        target = offset + 8 + displacement * 4
        _bootstrap_word(payload, target)
        record.update(operation="B", target_blob_file_offset=target)
    elif word & 0xffff0000 == 0xe92d0000:
        if not word & 0xffff or word & (1 << 13 | 1 << 15):
            raise FormatError("INIT reply PUSH register list is unsupported")
        record.update(operation="PUSH", base_register=13, register_mask=word & 0xffff,
                      byte_count=(word & 0xffff).bit_count() * 4)
    elif word & 0xffff0000 == 0xe8bd0000:
        if not word & (1 << 15) or word & (1 << 13 | 1 << 14):
            raise FormatError("INIT reply POP register list is unsupported")
        record.update(operation="POP", base_register=13, register_mask=word & 0xffff,
                      byte_count=(word & 0xffff).bit_count() * 4)
    elif word & 0xfff00ff0 == 0xe1d000b0:
        record.update(operation="LDRH", base_register=(word >> 16) & 15,
                      data_register=(word >> 12) & 15, byte_width=2,
                      byte_offset=((word >> 4) & 0xf0) | (word & 15))
    elif word & 0xfff00ff0 == 0xe1c000f0:
        register = (word >> 12) & 15
        if register & 1 or register > 12:
            raise FormatError("INIT reply STRD register pair is unsupported")
        record.update(operation="STRD", base_register=(word >> 16) & 15,
                      data_register=register, second_data_register=register + 1,
                      byte_width=8, byte_offset=((word >> 4) & 0xf0) | (word & 15))
    elif (word >> 26) & 3 == 0 and (word >> 21) & 15 in (2, 8):
        opcode = (word >> 21) & 15
        if (word >> 28 != 14 or (not word & (1 << 25) and word & 0x70) or
                bool(word & (1 << 20)) != (opcode == 8) or (opcode == 8 and word & 0xf000)):
            raise FormatError("INIT reply data operand is unsupported")
        record.update(operation="SUB" if opcode == 2 else "TST",
                      source_register=(word >> 16) & 15,
                      destination_register=(word >> 12) & 15)
        if word & (1 << 25):
            value, shift = word & 255, ((word >> 8) & 15) * 2
            record["immediate"] = ((value >> shift) | (value << ((32 - shift) % 32))) & 0xffffffff
        else:
            record.update(operand_register=word & 15, shift_kind="LSL", shift_amount=(word >> 7) & 31)
    else:
        return _fresh_init_arm_operand(payload, offset, expected)
    return record


def _init_reply_metadata_linkage(payload, images):
    """Conditional selected INIT reply metadata; no live pointer certification."""
    regions = _INIT_REPLY_REGIONS
    additional = sum(size for _, _, size, _ in regions)
    bridge_bytes = sum(len(expected) // 2 for _, _, expected in _COMMAND_BUFFER_BRIDGE_REGIONS) + 0x65c4
    fresh_bytes = sum(size for _, _, size, _ in _FRESH_INIT_REGIONS)
    dependency_bytes = bridge_bytes + fresh_bytes
    count = len(_INIT_REPLY_ARM_SITES) + len(_INIT_REPLY_ARC_SITES) + 1
    fresh_count = len(_FRESH_INIT_ARM_SITES) + len(_FRESH_INIT_ARC_SITES) + len(_FRESH_INIT_ARC_CALLS)
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            len(regions) > MAX_INIT_REPLY_REGIONS or additional > MAX_INIT_REPLY_BYTES or
            count > MAX_INIT_REPLY_ANCHORS or additional + dependency_bytes > MAX_INIT_REPLY_AGGREGATE_BYTES or
            len(_FRESH_INIT_REGIONS) > MAX_FRESH_INIT_REGIONS or fresh_bytes > MAX_FRESH_INIT_BYTES or
            dependency_bytes > MAX_FRESH_INIT_AGGREGATE_BYTES or fresh_count > MAX_FRESH_INIT_ANCHORS or
            MAX_FRESH_INIT_EVENTS < 35 or MAX_STOCK_HOST_COMMAND_CFG_STATES < 53 or
            len(_COMMAND_BUFFER_BRIDGE_REGIONS) + 1 > MAX_COMMAND_BUFFER_BRIDGE_REGIONS or
            bridge_bytes > MAX_COMMAND_BUFFER_BRIDGE_BYTES or
            0x65c4 // 12 > MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS):
        raise FormatError("INIT reply validation budget/identity exceeded")
    validated = []
    # New pins precede the dependency's own complete pre-pin and interpretation.
    for name, offset, size, digest in regions:
        if hashlib.sha256(bounded(payload, offset, size, "INIT reply metadata pin")).hexdigest() != digest:
            raise FormatError(f"INIT reply region {name} does not match the baseline")
        validated.append({"role": name, "blob_file_offset": offset, "size": size, "sha256": digest})
    fresh = _fresh_init_causal_contract(payload, images)
    bridge = fresh["bridge"]
    pinned = [(off, size) for _, off, size, _ in _FRESH_INIT_REGIONS]
    pinned += [(off, len(expected) // 2) for _, off, expected in _COMMAND_BUFFER_BRIDGE_REGIONS]
    if any(not any(low <= off and off + 4 <= low + size for low, size in pinned)
           for off, _ in _INIT_REPLY_ARM_SITES):
        raise FormatError("INIT reply operand escaped its complete dependency pins")
    arm = {off: _init_reply_arm_operand(payload, off, word) for off, word in _INIT_REPLY_ARM_SITES}
    anchors = [dict(r, architecture="ARM") for r in arm.values()]

    def imm(off):
        return arm[off]["immediate"]

    def disp(off):
        return arm[off]["byte_offset"]

    def branch(off, target, condition):
        if (arm[off]["operation"], arm[off]["target_blob_file_offset"], arm[off]["condition"]) != ("B", target, condition):
            raise FormatError("INIT reply selected branch is incoherent")

    def section(index):
        return struct.unpack("<10I", bounded(payload, 0x79540 + index * 40, 40, "INIT reply section"))

    sec = section(21)
    symbol = struct.unpack("<IIIBBH", bounded(payload, 0x6cd00, 16, "INIT reply symbol"))
    rela = struct.unpack("<IIi", bounded(payload, 0x72948, 12, "INIT reply relocation"))
    symtab, strtab, relasec, source = section(35), section(34), section(51), section(16)
    name = b"dms_deliver_info\0"
    if (sec != (0x260, 8, 3, 0x77138, 0x35734, 0x1654, 0, 0, 4, 1) or
            symbol != (0x1d8f, 0x78608, 24, 0x11, 0, 21) or rela != (0x246b8, 0x31904, 0) or
            0x2ea60 + symtab[4] + (rela[1] >> 8) * 16 != 0x6cd00 or
            0x2ea60 + strtab[4] + symbol[0] != 0x69824 or
            bounded(payload, 0x69824, len(name), "INIT reply symbol name") != name or
            (symtab[1], symtab[6], symtab[9], strtab[1], relasec[1], relasec[6], relasec[7]) != (2, 34, 16, 3, 4, 35, 16) or
            not sec[3] <= symbol[1] <= sec[3] + sec[5] - symbol[2]):
        raise FormatError("INIT reply section/symbol/RELA ownership is incoherent")

    # Selected baseline callbacks supply outer slot 0's metadata word, not an
    # arbitrary descriptor. Their construction/identity is an inherited premise.
    catalog_offset = _bootstrap_word(payload, 0xcfbcc)
    fallback_frame = arm[0x27bd0]["byte_count"] + imm(0x27bd4)
    loader_frame = arm[0x2af2c]["byte_count"] + imm(0x2af30)
    saved_r2_offset = imm(0x27bd4) + (arm[0x27bd0]["register_mask"] & 3).bit_count() * 4
    if (catalog_offset != bridge["image_placements"][0]["image_base_offset_from_B"] or
            disp(0x280a4) != bridge["allocation"]["physical_pointer_context_offset"] or
            arm[0x280a4]["data_register"] != arm[0x27d60]["data_register"] or
            disp(0x27d48) != saved_r2_offset or disp(0x27d4c) != disp(0x27c84) or
            disp(0x27c80) != 12 or disp(0x27d60) != 0 or
            arm[0x27d60]["base_register"] != 13 or disp(0x2afb0) != loader_frame or
            arm[0x2af3c]["operand_register"] != arm[0x27d64]["destination_register"] or
            arm[0x2af3c]["destination_register"] != arm[0x2afac]["data_register"] or
            arm[0x2afb0]["data_register"] != arm[0x2afb4]["data_register"] or
            arm[0x28058]["destination_register"] != arm[0x280a4]["base_register"] or
            arm[0x280ac]["source_register"] != 13 or
            arm[0x280ac]["destination_register"] != arm[0x27be0]["operand_register"] or
            arm[0x27be0]["destination_register"] != arm[0x27d54]["base_register"] or
            arm[0x27d54]["base_register"] != arm[0x27d5c]["base_register"] or
            arm[0x27c7c]["base_register"] != 13 or
            arm[0x27c7c]["data_register"] != arm[0x27c80]["base_register"]):
        raise FormatError("INIT reply selected loader argument/frame provenance is incoherent")
    original_base = imm(0x27d64)
    branch(0x2a4a0, 0x2a4c0, 1)
    branch(0x2a4c4, 0x2a4cc, 3)
    branch(0x2a4d4, 0x2a4dc, 9)
    branch(0x2a4f0, 0x2a500, 0)
    stride = (1 + (1 << arm[0x2a48c]["shift_amount"])) * (1 << arm[0x2a494]["shift_amount"])
    if (imm(0x2a490) != 0x40 or stride != 40 or disp(0x2a498) != 12 or
            imm(0x2a49c) != 0 or not sec[3] or not sec[3] < imm(0x2a4c0) or
            disp(0x2a4cc) != disp(0x2a500) or disp(0x2a4cc) != disp(0x2afac) or
            sec[3] < original_base or disp(0x2a4e8) != 8 or sec[2] & imm(0x2a4ec) or
            disp(0x2a508) != disp(0x2afb4) or arm[0x2a504]["operation"] != "SUB" or
            arm[0x2a50c]["operation"] != "ADD" or arm[0x2a514]["shift_amount"] != 2 or
            any(arm[a][k] != arm[b][k] for a, b, keys in (
                (0x2a48c, 0x2a4dc, ("source_register", "destination_register", "operand_register", "shift_amount")),
                (0x2a490, 0x2a4e0, ("source_register", "destination_register", "immediate")),
                (0x2a494, 0x2a4e4, ("source_register", "destination_register", "operand_register", "shift_amount"))) for k in keys) or
            arm[0x2a4e4]["destination_register"] != arm[0x2a4e8]["base_register"]):
        raise FormatError("INIT reply selected non-executable placement is incoherent")
    destination = sec[3] - original_base + catalog_offset
    if (disp(0x2a438) != 4 or disp(0x2a43c) != disp(0x2a498) or
            disp(0x2a444) != 14 or imm(0x2a448) != imm(0x2a510) or
            arm[0x2a440]["operation"] != "SUB" or arm[0x2a44c]["shift_amount"] != 2 or
            arm[0x2a450]["operation"] != "ADD" or disp(0x2a454) != disp(0x2a438) or
            disp(0x2a428) != disp(0x2a444) or imm(0x2a430) != imm(0x2a490) or
            (1 + (1 << arm[0x2a42c]["shift_amount"])) * (1 << arm[0x2a434]["shift_amount"]) != stride or
            arm[0x2a434]["destination_register"] != arm[0x2a43c]["base_register"]):
        raise FormatError("INIT reply symbol rebasing is incoherent")
    relative = symbol[1] - sec[3]
    rebased = destination + relative

    def position(address):
        if source[1] != 1 or not source[2] & 4 or not source[3] <= address <= source[3] + source[5] - 4:
            raise FormatError("INIT reply ARC operand escaped its executable section")
        return 0x2ea60 + source[4] + address - source[3]

    arc = {}
    for address, expected in _INIT_REPLY_ARC_SITES:
        off = position(address)
        word = _bootstrap_word(payload, off)
        if word != expected or not any(low <= off and off + 4 <= low + size for low, size in pinned):
            raise FormatError("INIT reply ARC operand does not match its body pin")
        record = {"architecture": "ARC", "section_index": 16, "elf_virtual_address": address,
                  "blob_file_offset": off, "word": word, "decode_conditional": True,
                  "destination_register": (word >> 21) & 63, "source_register": (word >> 15) & 63,
                  "operand_register": (word >> 9) & 63, "signed_low9": (word & 511) - (512 if word & 256 else 0)}
        record["operation"] = ("MOV register" if address in (0x258b4, 0x24604) else
                               "MOV LIMM" if address == 0x246b4 else "ADD immediate" if address == 0x246c0 else "STR")
        if address == 0x246b4:
            record["literal_value"] = _bootstrap_word(payload, position(address + 4))
        arc[address] = record
        anchors.append(record)
    relas = [struct.unpack_from("<IIi", payload, 0x72780 + i * 12) for i in range(2171)]
    owner = struct.unpack("<IIIBBH", bounded(payload, 0x69e50, 16, "INIT reply source function"))
    if (owner[5] != 16 or owner[3] & 15 != 2 or not owner[1] <= 0x246b4 < 0x246c8 <= owner[1] + owner[2] or
            [(0x72780 + i * 12, r) for i, r in enumerate(relas) if 0x246b4 <= r[0] < 0x246c8] != [(0x72948, rela)] or
            arc[0x246b4]["literal_value"] != symbol[1] or
            arc[0x246b4]["destination_register"] != arc[0x246bc]["operand_register"] or
            arc[0x246c0]["source_register"] != arc[0x246b4]["destination_register"] or
            arc[0x246c0]["destination_register"] != arc[0x246c4]["operand_register"] or
            arc[0x246bc]["source_register"] != arc[0x246c4]["source_register"] or
            arc[0x246bc]["signed_low9"] != disp(0x273bc) or arc[0x246c4]["signed_low9"] != disp(0x273d4)):
        raise FormatError("INIT reply selected literal/store dataflow is incoherent")
    functions = {s["name"]: s for s in fresh["outer_path"]["symbols"]}
    dispatch = next(r for r in fresh["relocation_receipts"] if r["caller"] == "Core_Command" and r["callee"] == "CmdInitialize")
    inherited_packet = next(r for r in fresh["instruction_anchors"] if r.get("elf_virtual_address") == 0x25824)
    for address, function in ((0x258b4, "Core_Command"), (0x24604, "CmdInitialize")):
        f = functions[function]
        if (f["section_index"] != 16 or not f["elf_virtual_address"] <= address <= f["elf_virtual_address"] + f["size"] - 4 or
                any(r[0] == address for r in relas)):
            raise FormatError("INIT reply buffer MOV has incorrect function/relocation ownership")
        arc[address]["source_function"] = function
    if (not dispatch["delay_slot"] or dispatch["delay_slot_semantics"] != "always executed" or
            dispatch["source_elf_virtual_address"] + 4 != arc[0x258b4]["elf_virtual_address"] or
            dispatch["delay_slot_word"] != arc[0x258b4]["word"] or
            inherited_packet["destination_register"] != arc[0x258b4]["source_register"] or
            inherited_packet["literal_value"] != fresh["outer_path"]["local_packet_address"] or
            any(arc[a]["source_register"] != arc[a]["operand_register"] for a in (0x258b4, 0x24604)) or
            arc[0x258b4]["destination_register"] != arc[0x24604]["source_register"] or
            arc[0x24604]["destination_register"] != arc[0x246bc]["source_register"]):
        raise FormatError("INIT reply selected outer buffer identity is incoherent")
    relocation = {"architecture": "ELF", "operation": "selected type4 S+A",
                  "relocation_record_blob_file_offset": 0x72948, "relocation_section_index": 51,
                  "vendor_type": rela[1] & 255, "symbol_index": rela[1] >> 8, "addend": rela[2],
                  "source_section_index": 16, "source_function": "CmdInitialize",
                  "source_function_symbol_index": 46, "source_function_elf_virtual_address": owner[1],
                  "source_function_bytes": owner[2],
                  "symbol_section_index": symbol[5], "literal_blob_file_offset": position(rela[0]),
                  "literal_elf_virtual_address": rela[0], "original_literal": symbol[1],
                  "patched_literal_offset_from_B": rebased + rela[2], "selected_relocation_is_unique": True,
                  "byte_store_blob_file_offsets": bridge["outer_pointer_relocation"]["byte_store_blob_file_offsets"],
                  "applied_before_section_copy": bridge["outer_pointer_relocation"]["applied_before_section_copy"]}
    anchors.append(relocation)
    inherited = {r["blob_file_offset"]: r for r in bridge["instruction_anchors"]}
    if (inherited[0x2b010]["target_blob_file_offset"] != 0x2a474 or
            inherited[0x2b018]["target_blob_file_offset"] != 0x2a3e0 or
            inherited[0x2b010]["blob_file_offset"] >= inherited[0x2b018]["blob_file_offset"]):
        raise FormatError("INIT reply placement/rebase order is incoherent")
    response_offset = imm(0x271e4)
    if (response_offset != imm(0x27244) or arm[0x271e4]["source_register"] != 13 or
            arm[0x27244]["source_register"] != 13 or
            arm[0x2720c]["operand_register"] != arm[0x271e4]["destination_register"] or
            arm[0x273bc]["base_register"] != arm[0x2720c]["destination_register"] or
            arm[0x273d4]["base_register"] != arm[0x273bc]["base_register"] or
            disp(0x273c0) != disp(0x273d8) or
            any(arm[off]["target_blob_file_offset"] != bridge["initialized_heap"]["translation_entry_blob_file_offset"] for off in (0x273c8, 0x273e0)) or
            arm[0x273cc]["destination_register"] != 0 or arm[0x273e4]["data_register"] != 0):
        raise FormatError("INIT reply ARM alias/consumer dataflow is incoherent")
    branch(0x1fddc, 0x1fdf4, 3)
    branch(0x1fdec, 0x1fdf4, 8)
    branch(0x1fdf0, 0x1fe60, 14)
    branch(0x1fdfc, 0x1fe58, 0)
    branch(0x1fe68, 0x1fe5c, 14)
    if (arm[0x1fdc0]["operation"] != "ADD" or arm[0x1fdc8]["operation"] != "SUB" or
            disp(0x1fdcc) != 0 or arm[0x1fdcc]["base_register"] != 2 or
            disp(0x1fdd0) != 0 or disp(0x1fde0) != 0 or imm(0x1fdf8) != 0 or
            arm[0x1fdb0]["destination_register"] != arm[0x1fdb8]["operand_register"] or
            arm[0x1fdb8]["destination_register"] != arm[0x1fdbc]["base_register"] or
            arm[0x1fdbc]["base_register"] != arm[0x1fdc4]["base_register"] or
            arm[0x1fdac]["register_mask"] & 0x3fff != arm[0x1fe5c]["register_mask"] & 0x3fff or
            arm[0x1fdac]["byte_count"] != arm[0x1fe5c]["byte_count"] or
            not arm[0x1fe5c]["register_mask"] & (1 << 15)):
        raise FormatError("INIT reply translation/store-before-check dataflow is incoherent")
    translation = {"virtual_base_offset": disp(0x1fdbc), "physical_base_offset": disp(0x1fdc4),
                   "inclusive_low_offset": disp(0x1fdd4), "inclusive_high_offset": disp(0x1fde4),
                   "chain_head_offset": disp(0x1fdf4), "error_status": imm(0x1fe58),
                   "success_status": imm(0x1fe64), "arithmetic_bits": 32,
                   "unsigned_inclusive_checks": True, "output_store_blob_file_offset": 0x1fdcc,
                   "bounds_check_blob_file_offsets": [0x1fdd8, 0x1fde8],
                   "fallback_chain_evaluated": False, "error_projection_requires_empty_chain": True}
    return {"basis": dict(fresh["basis"]),
            "validation": {"additional_region_count": len(regions), "additional_byte_count": additional,
                           "additional_anchor_count": len(anchors), "dependency_byte_count": dependency_bytes,
                           "aggregate_byte_count": additional + dependency_bytes, "validated_regions": validated},
            "fresh_init": fresh,
            "section_placement": {"section_index": 21, "section_type": sec[1], "flags": sec[2],
                                  "virtual_address": sec[3], "byte_extent": sec[5], "original_virtual_base": original_base,
                                  "initialized_contents_proven": False,
                                  "physical_base_offset_from_B": catalog_offset, "destination_offset_from_B": destination,
                                  "nonzero_va_bypasses_progbits_gate": True,
                                  "destination_table_context_offset": imm(0x2a510), "section_header_stride": stride,
                                  "fallback_frame_bytes": fallback_frame, "loader_frame_bytes": loader_frame,
                                  "catalog_callbacks_are_inherited_successful_baseline_premise": True,
                                  "catalog_metadata_word_blob_file_offset": 0xcfbcc,
                                  "constructed_descriptor_stack_offset": disp(0x27c7c),
                                  "caller_output_stack_offset": imm(0x280ac),
                                  "selected_entry_requires_valid_loader_and_section_iteration": True,
                                  "placement_precedes_symbol_rebase": True},
            "symbol_rebase": {"name": name[:-1].decode("ascii"), "symbol_index": rela[1] >> 8,
                              "symbol_record_blob_file_offset": 0x6cd00, "value": symbol[1], "size": symbol[2],
                              "section_index": symbol[5], "section_relative_offset": relative,
                              "destination_offset_from_B": destination, "rebased_offset_from_B": rebased,
                              "selected_entry_requires_valid_loader_and_symbol_iteration": True},
            "relocation_receipt": relocation,
            "outer_reply": {"word0_command": fresh["host_init"]["internal_command"],
                            "word1_status": fresh["outer_path"]["reply_backend_status"],
                            "word2_interpretation": "raw_uninterpreted",
                            "conditional_on_selected_fresh_arc_execution": True,
                            "transport_checks_metadata_words": False,
                            "local_packet_address": inherited_packet["literal_value"],
                            "caller_buffer_register": inherited_packet["destination_register"],
                            "callee_buffer_register": arc[0x24604]["destination_register"],
                            "buffer_argument_receipt_addresses": [0x258b4, 0x24604],
                            "word3_offset_from_B": rebased + rela[2],
                            "word4_offset_from_B": rebased + rela[2] + arc[0x246c0]["signed_low9"],
                            "word4_delta_bytes": arc[0x246c0]["signed_low9"]},
            "arm_translations": {"reply_base_stack_offset": response_offset,
                                 "response_alias_register": arm[0x2720c]["destination_register"],
                                 "context_register": arm[0x271d0]["destination_register"],
                                 "map_context_offset": disp(0x273c0),
                                 "output_context_offsets": [imm(0x273c4), imm(0x273dc)],
                                 "reply_byte_offsets": [disp(0x273bc), disp(0x273d4)],
                                 "helper_entry": arm[0x273c8]["target_blob_file_offset"],
                                 "helper_store_precedes_bounds_check": 0x1fdcc < min(translation["bounds_check_blob_file_offsets"]),
                                 "helper_status_checked": False, "stored_outputs_validated": False,
                                 "ignored_return_overwrite_blob_file_offsets": [0x273cc, 0x273e4],
                                 "saved_transport_status_preserved": fresh["arm_builder"]["preserves_transport_status_on_both_return_edges"],
                                 "raw_version_reply_byte_offset": disp(0x273e4),
                                 "raw_version_context_offset": disp(0x273e8), "translation": translation},
            "instruction_anchors": anchors,
            "assumptions": fresh["assumptions"] + [
                "Inherited successful baseline catalog callbacks select outer slot0, copy metadata word0 into the constructed descriptor and return that object; this addition does not decode arbitrary catalog callback behavior.",
                "Selected entries have valid allocated loader identity: section iteration r0=loader/r1=21 and symbol iteration r0=symbol793/r1=loader, with intact tables and ordinary callee-saved ABI preservation. This is not general loader/callback closure.",
                "Ordinary nested ARC callees preserve CmdInitialize's saved r14 reply-buffer register through the selected metadata stores.",
                "Ordinary map storage, C+0x250/C+0x254 output slots and protected stack storage are disjoint; map fields remain stable while the helper stores its output and then reads bounds or chain fields.",
                "Selected type4 relocation and section copy complete; NOBITS address placement proves no initialized object contents, and unselected vendor relocation/initialization behavior remains conditional.",
                "Reply metadata follows the selected fresh ARC INIT path only under the inherited serialized execution, visibility, calling-convention and freshness premises; matching declarations alone do not validate a live allocation or queue."],
            "validation_scope": {"conditional_reply_metadata": True, "catalog_callback_closure": False,
                                 "fallback_translation_chain_evaluated": False, "stored_queue_pointers_validated": False,
                                 "hardware_aliasing_proven": False, "runtime_observed": False,
                                 "freshness_proven": False, "operational_coherence_proven": False,
                                 "source_plane_lease": False, "active_decode_context": False,
                                 "standalone_execution": False, "public_route": False}}


def _init_reply_translation_projection(contract, scenario):
    """Initial-map u32 translation with an empty fallback chain, test-only.

    Contract must be the trusted result of the private metadata validator.
    Stable map fields and disjoint map/output/caller-frame storage are assumed,
    not established at runtime; aliases are outside this selected projection.
    """
    if type(contract) is not dict or type(contract.get("arm_translations")) is not dict:
        raise FormatError("INIT reply projection fixed-model container is invalid")
    arm = contract["arm_translations"]
    if type(arm.get("translation")) is not dict:
        raise FormatError("INIT reply projection translation container is invalid")
    t = arm["translation"]
    fixed = {"arithmetic_bits": 32, "success_status": 0, "error_status": 2,
             "virtual_base_offset": 0x28, "physical_base_offset": 0x30,
             "inclusive_low_offset": 0x18, "inclusive_high_offset": 0x1c, "chain_head_offset": 4}
    if (any(type(t.get(k)) is not int or t[k] != v for k, v in fixed.items()) or
            t.get("unsigned_inclusive_checks") is not True or
            arm.get("helper_store_precedes_bounds_check") is not True or
            arm.get("helper_status_checked") is not False or arm.get("stored_outputs_validated") is not False):
        raise FormatError("INIT reply projection fixed-model invariants do not match")
    try:
        heap = contract["fresh_init"]["bridge"]["initialized_heap"]
        defaults = {"physical_input": 0, "virtual_base": heap["virtual_base"],
                    "physical_base": heap["physical_base"], "inclusive_low": heap["inclusive_virtual_range"][0],
                    "inclusive_high": heap["inclusive_virtual_range"][1], "chain_head": 0}
    except (KeyError, TypeError, IndexError) as error:
        raise FormatError("INIT reply projection initialized-map defaults are invalid") from error
    if type(scenario) is not dict or set(scenario) - set(defaults):
        raise FormatError("INIT reply translation has unsupported scenario fields")
    values = dict(defaults, **scenario)
    if any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values.values()):
        raise FormatError("INIT reply translation scalar is not u32")
    if values["chain_head"] != 0:
        raise FormatError("INIT reply translation projection does not evaluate fallback chains")
    output = (values["virtual_base"] + values["physical_input"] - values["physical_base"]) & ((1 << t["arithmetic_bits"]) - 1)
    admitted = values["inclusive_low"] <= output <= values["inclusive_high"]
    return {"stored_output": output, "helper_status": t["success_status"] if admitted else t["error_status"],
            "within_initial_map": admitted, "store_precedes_bounds_check": contract["arm_translations"]["helper_store_precedes_bounds_check"],
            "caller_checks_helper_status": contract["arm_translations"]["helper_status_checked"],
            "stored_output_validated": contract["arm_translations"]["stored_outputs_validated"],
            "fallback_chain_evaluated": False, "chain_head": 0, "runtime_observed": False}


def _open_reply_arm_operand(payload, offset, expected):
    """Fixed A32 operands needed by the selected ordinary OPEN path."""
    word = _bootstrap_word(payload, offset)
    if word != expected:
        raise FormatError("OPEN ARM operand does not match the baseline")
    record = {"blob_file_offset": offset, "word": word, "condition": word >> 28}
    if word & 0x0ffffff0 == 0x012fff10:
        record.update(operation="BX register", operand_register=word & 15)
    elif word & 0xfff00000 == 0xe3000000:
        record.update(operation="MOVW", destination_register=(word >> 12) & 15,
                      immediate=((word >> 4) & 0xf000) | (word & 0xfff))
    elif (word >> 25) & 7 == 5 and word >> 28 != 15:
        displacement = word & 0xffffff
        if displacement & 0x800000:
            displacement -= 1 << 24
        record.update(operation="BL" if word & (1 << 24) else "B",
                      target_blob_file_offset=offset + 8 + displacement * 4)
    elif word & 0x0fc000f0 == 0x00000090:
        if word >> 28 != 14 or word & ((1 << 21) | (1 << 20)):
            raise FormatError("OPEN multiply shape is unsupported")
        record.update(operation="MUL", destination_register=(word >> 16) & 15,
                      source_register=word & 15, operand_register=(word >> 8) & 15,
                      sets_flags=False)
    elif word & 0xfff000f0 in (0xe1c000d0, 0xe1c000f0):
        register = (word >> 12) & 15
        if register & 1 or register > 12:
            raise FormatError("OPEN doubleword register pair is unsupported")
        record.update(operation="LDRD" if word & 0xf0 == 0xd0 else "STRD",
                      base_register=(word >> 16) & 15, data_register=register,
                      second_data_register=register + 1, byte_width=8,
                      byte_offset=((word >> 4) & 0xf0) | (word & 15))
    elif (word >> 25) & 7 == 4 and word & 0xffff0000 not in (0xe92d0000, 0xe8bd0000):
        if word >> 28 != 14 or not word & (1 << 23) or word & ((1 << 22) | (1 << 21)):
            raise FormatError("OPEN multiple-register addressing is unsupported")
        mask = word & 0xffff
        if not mask or mask & ((1 << 13) | (1 << 15)):
            raise FormatError("OPEN multiple-register list is unsupported")
        record.update(operation="LDM" if word & (1 << 20) else "STM",
                      base_register=(word >> 16) & 15, register_mask=mask,
                      byte_count=mask.bit_count() * 4, writeback=False,
                      addressing="increment before" if word & (1 << 24) else "increment after")
    else:
        record.update(_init_reply_arm_operand(payload, offset, expected))
    return record


def _open_reply_arc_operand(payload, section, address, offset, expected, owner_end):
    """Conditional legacy ARC fields; neither a vendor decoder nor execution."""
    word = _bootstrap_word(payload, offset)
    if word != expected:
        raise FormatError("OPEN ARC operand does not match the baseline")
    major = word >> 27
    labels = {0: "LD indexed", 1: "LD", 2: "ST", 3: "EXT", 4: "B", 5: "BL",
              6: "LP", 7: "J", 8: "ADD", 9: "ADC", 10: "SUB", 11: "SBC",
              12: "AND", 13: "OR", 14: "BIC", 15: "XOR", 16: "ASL"}
    if major not in labels:
        raise FormatError("OPEN ARC opcode is outside the selected base model")
    record = {"architecture": "ARC", "section_index": section,
              "elf_virtual_address": address, "blob_file_offset": offset, "word": word,
              "decode_conditional": True, "opcode_major": major, "operation": labels[major],
              "destination_register": (word >> 21) & 63, "source_register": (word >> 15) & 63,
              "operand_register": (word >> 9) & 63, "low9": word & 511,
              "signed_low9": (word & 511) - (512 if word & 256 else 0)}
    source, operand = record["source_register"], record["operand_register"]
    if major == 12 and source == operand:
        record["operation"] = "MOV"
    if major in (4, 5, 6):
        displacement = (word >> 7) & 0xfffff
        if displacement & (1 << 19):
            displacement -= 1 << 20
        record.update(condition=word & 31, target_elf_virtual_address=address + 4 + displacement * 4,
                      pc_bias_bytes=4)
    if major in (4, 5, 7):
        delay = "taken only" if word & 64 else "always executed" if word & 32 else "none"
        record["delay_slot_semantics"] = delay
        if delay != "none":
            if offset + 8 > owner_end:
                raise FormatError("OPEN ARC delay escaped its complete owner pin")
            record.update(delay_slot_elf_virtual_address=address + 4,
                          delay_slot_word=_bootstrap_word(payload, offset + 4))
    limm = ((major in (0, 8, 9, 10, 11, 12, 13, 14, 15, 16) and 62 in (source, operand)) or
            (major == 1 and source == 62) or (major == 2 and operand == 62))
    if limm:
        if offset + 8 > owner_end:
            raise FormatError("OPEN ARC LIMM escaped its complete owner pin")
        record.update(literal_value=_bootstrap_word(payload, offset + 4), literal_blob_file_offset=offset + 4)
    if major == 16 and operand == 63:
        record["shift_amount"] = record["signed_low9"]
    if address == 0x24b48:
        record["vendor_ISA_validated"] = False
    return record


def _open_reply_preflight(payload):
    """Conservative complete dependency union, before any firmware interpretation."""
    bridge_bytes = sum(len(raw) // 2 for _, _, raw in _COMMAND_BUFFER_BRIDGE_REGIONS) + 0x65c4
    fresh_bytes = sum(size for _, _, size, _ in _FRESH_INIT_REGIONS)
    init_bytes = sum(size for _, _, size, _ in _INIT_REPLY_REGIONS)
    inner_count = len(_INNER_DESCRIPTOR_HEADERS) + len(_INNER_DESCRIPTOR_SECTIONS) + len(_INNER_DESCRIPTOR_WINDOWS)
    inner_count += sum(name != ".shstrtab" for _, _, _, _, _, name in _INNER_DESCRIPTOR_SECTIONS)
    inner_bytes = sum(len(raw) // 2 for _, _, raw in _INNER_DESCRIPTOR_HEADERS)
    inner_bytes += sum(len(raw) // 2 + (len(name) + 1 if name != ".shstrtab" else 0)
                       for _, _, _, raw, _, name in _INNER_DESCRIPTOR_SECTIONS)
    inner_bytes += sum(len(raw) // 2 for _, _, _, _, _, raw in _INNER_DESCRIPTOR_WINDOWS)
    new_bytes = sum(size for _, _, size, _ in _OPEN_REPLY_REGIONS)
    instructions = len(_OPEN_REPLY_ARM_SITES) + len(_OPEN_REPLY_ARC_SITES)
    elf_count = len(_OPEN_REPLY_SYMBOLS) + 2
    owned_count = len(_OPEN_REPLY_OWNED_RELOCATIONS)
    fresh_count = len(_FRESH_INIT_ARM_SITES) + len(_FRESH_INIT_ARC_SITES) + len(_FRESH_INIT_ARC_CALLS)
    init_count = len(_INIT_REPLY_ARM_SITES) + len(_INIT_REPLY_ARC_SITES) + 1
    dependency_bytes = bridge_bytes + fresh_bytes + init_bytes + inner_bytes
    dependency_count = len(_COMMAND_BUFFER_BRIDGE_REGIONS) + 1 + len(_FRESH_INIT_REGIONS) + len(_INIT_REPLY_REGIONS) + inner_count
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            len(_OPEN_REPLY_REGIONS) > MAX_OPEN_REPLY_REGIONS or new_bytes > MAX_OPEN_REPLY_BYTES or
            new_bytes + dependency_bytes > MAX_OPEN_REPLY_AGGREGATE_BYTES or
            instructions > MAX_OPEN_REPLY_INSTRUCTIONS or elf_count > MAX_OPEN_REPLY_ELF_RECORDS or
            owned_count > MAX_OPEN_REPLY_OWNED_RELOCATIONS or
            instructions + elf_count + owned_count > MAX_OPEN_REPLY_SEMANTIC_RECEIPTS or
            MAX_OPEN_REPLY_NEW_TABLE_RECORDS < 345 or MAX_OPEN_REPLY_RELOCATION_RECORDS < 2516 or
            len(_COMMAND_BUFFER_BRIDGE_REGIONS) + 1 > MAX_COMMAND_BUFFER_BRIDGE_REGIONS or
            bridge_bytes > MAX_COMMAND_BUFFER_BRIDGE_BYTES or MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS < 2171 or
            len(_FRESH_INIT_REGIONS) > MAX_FRESH_INIT_REGIONS or fresh_bytes > MAX_FRESH_INIT_BYTES or
            bridge_bytes + fresh_bytes > MAX_FRESH_INIT_AGGREGATE_BYTES or fresh_count > MAX_FRESH_INIT_ANCHORS or
            MAX_FRESH_INIT_EVENTS < 35 or MAX_STOCK_HOST_COMMAND_CFG_STATES < 53 or
            len(_INIT_REPLY_REGIONS) > MAX_INIT_REPLY_REGIONS or init_bytes > MAX_INIT_REPLY_BYTES or
            bridge_bytes + fresh_bytes + init_bytes > MAX_INIT_REPLY_AGGREGATE_BYTES or init_count > MAX_INIT_REPLY_ANCHORS or
            inner_count > MAX_INNER_DESCRIPTOR_REGIONS or inner_bytes > MAX_INNER_DESCRIPTOR_BYTES):
        raise FormatError("OPEN reply validation budget/identity exceeded")
    regions = list(_OPEN_REPLY_REGIONS)
    regions += [("init_reply:" + name, off, size, digest) for name, off, size, digest in _INIT_REPLY_REGIONS]
    regions += [("fresh_init:" + name, off, size, digest) for name, off, size, digest in _FRESH_INIT_REGIONS]
    regions += [("bridge:" + name, off, len(raw) // 2, hashlib.sha256(bytes.fromhex(raw)).hexdigest())
                for name, off, raw in _COMMAND_BUFFER_BRIDGE_REGIONS]
    regions.append(("bridge:text_relocations", 0x72780, 0x65c4, _COMMAND_BUFFER_BRIDGE_RELA_SHA256))
    regions += [(f"inner:elf_header_{slot}", off, len(raw) // 2, hashlib.sha256(bytes.fromhex(raw)).hexdigest())
                for slot, off, raw in _INNER_DESCRIPTOR_HEADERS]
    for slot, index, off, raw, name_off, name in _INNER_DESCRIPTOR_SECTIONS:
        regions.append((f"inner:section_{slot}_{index}", off, len(raw) // 2, hashlib.sha256(bytes.fromhex(raw)).hexdigest()))
        if name != ".shstrtab":
            encoded = name.encode("ascii") + b"\0"
            regions.append((f"inner:name_{slot}_{index}", name_off, len(encoded), hashlib.sha256(encoded).hexdigest()))
    regions += [("inner:" + name, off, len(raw) // 2, hashlib.sha256(bytes.fromhex(raw)).hexdigest())
                for _, name, _, _, off, raw in _INNER_DESCRIPTOR_WINDOWS]
    validated = []
    for name, off, size, digest in regions:
        if hashlib.sha256(bounded(payload, off, size, "OPEN reply complete union pin")).hexdigest() != digest:
            raise FormatError(f"OPEN reply region {name} does not match the baseline")
        validated.append({"role": name, "blob_file_offset": off, "size": size, "sha256": digest})
    return {"additional_region_count": len(_OPEN_REPLY_REGIONS), "additional_byte_count": new_bytes,
            "dependency_region_count": dependency_count, "dependency_byte_count": dependency_bytes,
            "aggregate_region_count": len(regions), "aggregate_byte_count": new_bytes + dependency_bytes,
            "arm_instruction_count": len(_OPEN_REPLY_ARM_SITES), "arc_instruction_count": len(_OPEN_REPLY_ARC_SITES),
            "instruction_count": instructions, "fixed_elf_record_count": elf_count,
            "owned_relocation_count": owned_count, "scanned_relocation_count": 2516,
            "additional_semantic_count": instructions + elf_count + owned_count,
            "validated_regions": validated[:len(_OPEN_REPLY_REGIONS)],
            "dependency_regions": validated[len(_OPEN_REPLY_REGIONS):]}


def _open_reply_metadata_linkage(payload, images):
    """Selected ordinary OPEN metadata provenance, conditional and offline only."""
    validation = _open_reply_preflight(payload)
    if (not isinstance(images, (list, tuple)) or len(images) != 2 or
            any(type(i) is not dict for i in images) or
            [(i.get("blob_file_offset"), i.get("blob_file_end"), i.get("section_count")) for i in images] !=
            [(0x2ea60, 0x79dd8, 55), (0x79dd8, 0xcfbb0, 112)] or
            any((i.get("class"), i.get("endianness"), i.get("machine"), i.get("elf_type"), i.get("flags")) !=
                (32, "little", 45, 2, 0) for i in images)):
        raise FormatError("OPEN metadata image identities do not match the baseline")
    # No helper or operand/ELF decoder runs until the complete conservative
    # union, including the otherwise later inner dependency, has been pinned.
    init = _init_reply_metadata_linkage(payload, images)
    inner = _inner_descriptor_map(payload, images)
    fresh = init["fresh_init"]
    lemma = inner["paths"]["record_pointer_and_boundary"]["record_pool_context_snapshot"]["conditional_constructor_return"]
    all_regions = validation["validated_regions"] + validation["dependency_regions"]

    def read(offset, size):
        if not any(r["blob_file_offset"] <= offset and offset + size <= r["blob_file_offset"] + r["size"]
                   for r in all_regions):
            raise FormatError("OPEN metadata read escaped the complete pinned union")
        return bounded(payload, offset, size, "OPEN metadata pinned read")

    def section(index):
        return struct.unpack("<10I", read(0x79540 + index * 40, 40))

    sections = {i: section(i) for i in (2, 4, 16, 21, 34, 35, 37, 39, 51)}
    if ((sections[34][1], sections[35][1], sections[35][6], sections[35][9]) != (3, 2, 34, 16) or
            any(sections[i][1] != 1 or not sections[i][2] & 4 for i in (2, 4, 16))):
        raise FormatError("OPEN metadata section/symbol table ownership is incoherent")
    functions = {r["name"]: dict(r) for r in fresh["outer_path"]["symbols"]}
    elf_receipts = []
    for name, index, owner, address, size, symbol_pos, name_pos in _OPEN_REPLY_SYMBOLS:
        symbol = struct.unpack("<IIIBBH", read(symbol_pos, 16))
        expected_info = 0x11 if name == "dm_return_info" else 2 if index in (47, 70) else 0x12
        encoded = name.encode("ascii") + b"\0"
        sec = sections[owner]
        if (symbol_pos != 0x2ea60 + sections[35][4] + index * 16 or
                symbol != (name_pos - 0x2ea60 - sections[34][4], address, size, expected_info, 0, owner) or
                read(name_pos, len(encoded)) != encoded or not sec[3] <= address <= sec[3] + sec[5] - size):
            raise FormatError("OPEN selected symbol/name/section identity is incoherent")
        r = {"name": name, "symbol_index": index, "symbol_record_blob_file_offset": symbol_pos,
             "name_blob_file_offset": name_pos, "section_index": owner, "elf_virtual_address": address,
             "size": size, "info": symbol[3], "other": symbol[4], "section_relative_offset": address - sec[3]}
        if name != "dm_return_info":
            r["blob_file_offset"] = 0x2ea60 + sec[4] + address - sec[3]
            functions[name] = r
        elf_receipts.append(r)
    tables = []
    for index, owner, count in ((37, 2, 84), (39, 4, 261), (51, 16, 2171)):
        sec = sections[index]
        if (sec[1], sec[2], sec[3], sec[5], sec[6], sec[7], sec[8], sec[9]) != (4, 0, 0, count * 12, 35, owner, 4, 12):
            raise FormatError("OPEN RELA table type/link/owner/stride is incoherent")
        offset = 0x2ea60 + sec[4]
        raw = read(offset, count * 12)
        records = [struct.unpack_from("<IIi", raw, i * 12) for i in range(count)]
        tables.extend((index, owner, offset + i * 12, *record) for i, record in enumerate(records))
        if index != 51:
            elf_receipts.append({"operation": "RELA section header", "section_index": index,
                                 "source_section_index": owner, "linked_symbol_table_index": sec[6],
                                 "blob_file_offset": 0x79540 + index * 40, "record_blob_file_offset": offset,
                                 "record_count": count, "entry_size": sec[9]})
    new_owners = {name: functions[name] for name, *_ in _OPEN_REPLY_SYMBOLS if name != "dm_return_info"}
    owned = []
    for table, owner, pos, source, info, addend in tables:
        matches = [name for name, f in new_owners.items() if f["section_index"] == owner and
                   f["elf_virtual_address"] <= source < f["elf_virtual_address"] + f["size"]]
        if len(matches) > 1:
            raise FormatError("OPEN RELA source has ambiguous function ownership")
        if matches:
            owned.append((matches[0], pos, source, info & 255, info >> 8, addend))
    if sorted(owned) != sorted(_OPEN_REPLY_OWNED_RELOCATIONS):
        raise FormatError("OPEN owned RELA inventory is missing, duplicated or changed")
    relocations = []
    table_at = {(owner, source): [] for _, owner, _, source, _, _ in tables}
    for table, owner, pos, source, info, addend in tables:
        table_at[owner, source].append((table, pos, info, addend))
    for name, pos, address, kind, target, addend in owned:
        f = functions[name]
        sec = sections[f["section_index"]]
        matches = table_at[f["section_index"], address]
        if len(matches) != 1:
            raise FormatError("OPEN selected RELA source is not unique within its owning table")
        relocations.append({"source_function": name, "source_function_symbol_index": f["symbol_index"],
                            "source_section_index": f["section_index"], "source_elf_virtual_address": address,
                            "source_blob_file_offset": 0x2ea60 + sec[4] + address - sec[3],
                            "relocation_section_index": matches[0][0], "relocation_record_blob_file_offset": pos,
                            "vendor_type": kind, "symbol_index": target, "addend": addend,
                            "unpinned_target_definition_interpreted": False, "runtime_application_proven": False})
    arm = {off: dict(_open_reply_arm_operand(payload, off, word), architecture="ARM")
           for off, word in _OPEN_REPLY_ARM_SITES}
    for off, r in arm.items():
        owners = [name for name, low, size, _ in _OPEN_REPLY_REGIONS[:5] if low <= off < low + size]
        r["source_function"] = (owners[0] if len(owners) == 1 else "fixed_context_getter" if off in (0x898, 0x89c)
                                else "selected_dispatch_call_site_only" if off == 0x62d8 else None)
        if r["source_function"] is None:
            raise FormatError("OPEN ARM operand escaped its selected function/call-site owner")
    arc = {}
    for owner, address, expected in _OPEN_REPLY_ARC_SITES:
        sec = sections[owner]
        matches = [f for f in functions.values() if f["section_index"] == owner and
                   f["elf_virtual_address"] <= address <= f["elf_virtual_address"] + f["size"] - 4]
        if len(matches) != 1:
            raise FormatError("OPEN ARC instruction has ambiguous or missing selected symbol owner")
        f = matches[0]
        off = 0x2ea60 + sec[4] + address - sec[3]
        read(off, 4)
        end = 0x2ea60 + sec[4] + f["elf_virtual_address"] - sec[3] + f["size"]
        arc[address] = _open_reply_arc_operand(payload, owner, address, off, expected, end)
        arc[address]["source_function"] = f["name"]
    anchors = list(arm.values()) + list(arc.values())
    # LIMM and delay words are raw data owned by the complete body pins, not
    # extra decoded instructions or silently followed external call targets.
    for r in anchors:
        for key in ("literal_blob_file_offset",):
            if key in r:
                read(r[key], 4)
        if r.get("delay_slot_semantics", "none") != "none":
            read(r["blob_file_offset"] + 4, 4)

    def ai(off):
        return arm[off]["immediate"]

    def ad(off):
        return arm[off]["byte_offset"]

    def av(address):
        return arc[address].get("literal_value", arc[address]["signed_low9"])

    def am(address):
        return arc[address]["signed_low9"]

    def ab(off, target, condition=14, operation="B"):
        r = arm[off]
        if (r["operation"], r["target_blob_file_offset"], r["condition"]) != (operation, target, condition):
            raise FormatError("OPEN selected ARM call/control edge is incoherent")

    for source, target in ((0x62d8, 0x51c8), (0x5730, 0x8a0), (0xb18, 0xa2a4),
                           (0xa318, 0xf7e4), (0xfb74, 0x27480), (0x27578, 0x2705c), (0x275ac, 0x1fdac)):
        ab(source, target, operation="BL")
    for source, target, cond in ((0x573c, 0x588c, 0), (0xa324, 0xa368, 0),
                                (0xfb0c, 0xfb1c, 0), (0xfb18, 0xfb94, 1),
                                (0xfb80, 0xfb94, 0), (0xfb90, 0xf830, 14)):
        ab(source, target, cond)
    global_literal = next(r for r in fresh["bridge"]["instruction_anchors"] if r.get("literal_blob_file_offset") == 0x6fc)
    if (arm[0x898]["literal_value"] != global_literal["literal_value"] or
            arm[0x89c]["operation"] != "BX register" or arm[0x89c]["operand_register"] != 14):
        raise FormatError("OPEN getter source identity is incoherent")
    edges = []
    for address, r in arc.items():
        rel = table_at.get((r["section_index"], address), [])
        if r["operation"] != "BL":
            if rel:
                raise FormatError("OPEN non-call instruction has an unexpected relocation")
            continue
        if len(rel) > 1 or (rel and rel[0][2] & 255 != 6):
            raise FormatError("OPEN selected ARC call relocation is incoherent")
        target = r["target_elf_virtual_address"]
        known = [f for f in functions.values() if f["elf_virtual_address"] == target]
        e = {"source_function": r["source_function"], "source_section_index": r["section_index"],
             "source_elf_virtual_address": address, "original_target_elf_virtual_address": target,
             "pc_bias_bytes": r["pc_bias_bytes"], "delay_slot_semantics": r["delay_slot_semantics"],
             "runtime_edge_proven": False, "target_definition_validated": bool(known)}
        if "delay_slot_word" in r:
            e["delay_slot_word"] = r["delay_slot_word"]
        if known:
            if len(known) != 1:
                raise FormatError("OPEN selected call target definition is ambiguous")
            f = known[0]
            e["target_function"] = f["name"]
            if rel:
                table, pos, info, addend = rel[0]
                if info >> 8 != f["symbol_index"]:
                    raise FormatError("OPEN call RELA names a different selected target")
                displacement = f["elf_virtual_address"] + addend - address - 4
                if displacement % 4 or not -(1 << 21) <= displacement < 1 << 21:
                    raise FormatError("OPEN selected type6 displacement escaped its signed field")
                patched = (r["word"] & 0xf800007f) | ((displacement << 5) & 0x07ffff80)
                if patched != r["word"]:
                    raise FormatError("OPEN selected type6 original call does not match S+A-P-4")
                e.update(relocation_record_blob_file_offset=pos, vendor_type=info & 255,
                         symbol_index=info >> 8, addend=addend, original_encoding_preserved_under_selected_rebase=True)
            elif f["section_index"] != r["section_index"]:
                raise FormatError("OPEN cross-section selected call lacks an owned relocation")
        elif rel:
            e.update(relocation_record_blob_file_offset=rel[0][1], vendor_type=rel[0][2] & 255,
                     symbol_index=rel[0][2] >> 8, addend=rel[0][3])
        edges.append(e)
    for source, target, delay in ((0x258bc, 0x24788, 0x258c0), (0x24aa4, 0x266f8, 0x24aa8),
                                  (0x9fd0, 0x9e74, 0x9fd4), (0xb980, 0xb554, 0xb984)):
        r = arc[source]
        if (r["target_elf_virtual_address"] != target or r["delay_slot_semantics"] != "always executed" or
                r["delay_slot_elf_virtual_address"] != delay or r["delay_slot_word"] != arc[delay]["word"]):
            raise FormatError("OPEN selected ARC call/delay linkage is incoherent")
    inherited_packet = next(r for r in fresh["instruction_anchors"] if r.get("elf_virtual_address") == 0x25824)
    if (arc[0x25890]["target_elf_virtual_address"] != 0x258bc or
            arc[0x258c4]["target_elf_virtual_address"] != 0x259c0 or
            arc[0x258c0]["source_register"] != inherited_packet["destination_register"] or
            arc[0x258c0]["destination_register"] != arc[0x247c4]["source_register"]):
        raise FormatError("OPEN dispatch/reply buffer identity is incoherent")
    reply_register = arc[0x247c4]["destination_register"]
    if any(arc[off]["source_register"] != reply_register for off in (0x24b0c, 0x24b14, 0x24b1c, 0x24b70)):
        raise FormatError("OPEN reply stores use a different selected buffer")
    object_record = next(r for r in elf_receipts if r.get("name") == "dm_return_info")
    placement = init["section_placement"]
    rebased = placement["destination_offset_from_B"] + object_record["section_relative_offset"]
    selected_literals = (0x24944, 0x24b6c, 0xbe5c)
    for address in selected_literals:
        rows = [r for r in relocations if r["source_elf_virtual_address"] == address]
        if (len(rows) != 1 or (rows[0]["vendor_type"], rows[0]["symbol_index"], rows[0]["addend"]) != (4, 794, 0) or
                struct.unpack("<I", read(rows[0]["source_blob_file_offset"], 4))[0] != object_record["elf_virtual_address"]):
            raise FormatError("OPEN selected return metadata type4 literal is incoherent")
        rows[0]["selected_target_name"] = "dm_return_info"
        rows[0]["patched_literal_offset_from_B"] = rebased
    frame = arm[0x27480]["byte_count"] + ai(0x27484)
    raw_reply = [(ad(load), ad(store)) for load, store in ((0x27580, 0x27584), (0x27588, 0x2758c), (0x27590, 0x27594))]
    if (ai(0x27484) != ai(0x275bc) or arm[0x27480]["byte_count"] != arm[0x275c0]["byte_count"] or
            ai(0x274a0) - ai(0x274a4) != ai(0x274a8) or ai(0x274a8) != ai(0x274b8) or
            ad(0x2757c) != ad(0x275b8) or ad(0x27508) - frame != ad(0xfb34) + 4 or
            ad(0x274dc) != am(0x248e8) or ad(0x2750c) != am(0x24878) or
            arm[0x274cc]["destination_register"] != arm[0x27580]["base_register"] or
            arm[0x275b0]["destination_register"] != 0 or ad(0x275a4) == init["arm_translations"]["map_context_offset"]):
        raise FormatError("OPEN packet frame/raw response provenance is incoherent")
    if (arm[0xfb70]["operand_register"] != arm[0xf7ec]["destination_register"] or
            arm[0xfb70]["destination_register"] != arm[0x27488]["operand_register"] or
            arm[0x27488]["destination_register"] != arm[0x275a4]["base_register"] or
            arm[0xfb6c]["destination_register"] != arm[0x2748c]["operand_register"] or
            arm[0xfb6c]["operand_register"] != arm[0xfbb0]["data_register"] or
            arm[0xf7f0]["destination_register"] != arm[0xfbb0]["base_register"] or
            arm[0x27568]["operand_register"] != arm[0x274a4]["destination_register"] or
            arm[0x2756c]["operand_register"] != arm[0x274a0]["destination_register"] or
            arm[0x2757c]["data_register"] != arm[0x275b8]["data_register"] or
            any(arm[load]["data_register"] != arm[store]["data_register"] or
                arm[store]["base_register"] != arm[0x2748c]["destination_register"]
                for load, store in ((0x27580, 0x27584), (0x27588, 0x2758c), (0x27590, 0x27594))) or
            arm[0xfb7c]["source_register"] != arm[0xfb78]["destination_register"] or
            ai(0xfb7c) != 0 or arm[0xfb8c]["operand_register"] != arm[0xfb78]["destination_register"] or
            arm[0xfbc4]["base_register"] != arm[0xfbb0]["base_register"] or
            arm[0xfbcc]["data_register"] != arm[0xfbc4]["data_register"] or
            arm[0xfbcc]["base_register"] != arm[0xfbc8]["data_register"] or
            arm[0xfb08]["source_register"] != arm[0xfb68]["operand_register"] or ai(0xfb08) != 0):
        raise FormatError("OPEN selected C/H, saved status or publication register linkage is incoherent")
    count_max = av(0x2487c)
    source_init = lemma["source_initialization"]
    f_offset = source_init["pool_context_offset_under_conditions"]
    base_context = av(0x267a8)
    ring_fields = [base_context + am(0x2692c), base_context + am(0x26958)]
    ring_offsets_from_f = [av(0x26924), av(0x26938)]
    metadata_from_f = av(0x268e0)
    metadata_field = av(0x267b8) + am(0x268e8)
    if (count_max > source_init["earlier_variable_loop"]["maximum_nonclobbering_count"] or
            arc[0x24880]["condition"] != 14 or arc[0x24880]["target_elf_virtual_address"] != 0x24898 or
            am(0x2496c) != am(0x24878) or arc[0x2496c]["source_register"] != reply_register or
            arc[0x2496c]["destination_register"] != arc[0x24a8c]["source_register"] or
            arc[0x26734]["source_register"] != arc[0x24a8c]["destination_register"] or
            av(0x26748) != source_init["context_end_offset"] or
            av(0x3b308) * 2 != source_init["size_bytes_under_base_model"] or
            base_context + am(0x267b0) != 0x530 or
            am(0x26918) != am(0x267b0) or am(0x268c8) != am(0x267b0) or
            ring_fields != [av(0x24abc) + am(0x24b10), av(0x24abc) + am(0x24b18)] or
            arc[0x26924]["source_register"] != arc[0x26918]["destination_register"] or
            arc[0x26938]["source_register"] != arc[0x26918]["destination_register"]):
        raise FormatError("OPEN conditional pool/ring field derivation is incoherent")
    ring_bytes = ring_offsets_from_f[1] - ring_offsets_from_f[0]
    if (ring_bytes != (av(0xb5a8) + 1) * (1 << av(0xb5e0)) or
            av(0x24b40) != av(0xb588) or av(0x24b60) != av(0xb588) or
            arc[0x24b08]["operation"] != "SUB" or
            len({arc[0x24b08][k] for k in ("destination_register", "source_register", "operand_register")}) != 1 or
            arc[0x24948]["operation"] != "SUB" or
            len({arc[0x24948][k] for k in ("destination_register", "source_register", "operand_register")}) != 1):
        raise FormatError("OPEN selected zero/status and ring header/index units are incoherent")
    channel_stride = 1 << arc[0x2493c]["shift_amount"]
    if (av(0x24940) != object_record["elf_virtual_address"] or
            av(0x24b68) != object_record["elf_virtual_address"] or
            av(0x24b48) != arc[0x2493c]["shift_amount"] or object_record["size"] % channel_stride):
        raise FormatError("OPEN return metadata channel-index scale is incoherent")
    activated_base = av(0x9fc8) + am(0x9fd4)
    activated_delivery = activated_base + ring_fields[0]
    activated_return = activated_base + ring_fields[1]
    activated_pool = activated_base + metadata_field
    table_base = av(0x26780) + am(0x26788)
    if (table_base != av(0x9fb4) + am(0x9fbc) or
            arc[0x26790]["shift_amount"] != arc[0x9fa8]["shift_amount"] or
            av(0x9fc0) != source_init["context_end_offset"] or
            av(0xb688) + am(0xb97c) != activated_delivery or
            av(0xb818) + am(0xb8dc) != activated_pool or
            av(0x26158) + am(0x26174) != activated_delivery or
            av(0x26158) + am(0x261e4) != activated_return):
        raise FormatError("OPEN conditional activation/consumer address linkage is incoherent")
    ppb_stride = (((1 << av(0xb8e4)) - 1) * (1 << av(0xb8ec)) + 1) * (1 << av(0xb8f4))
    ppb_bytes = av(0xb900)
    if ppb_stride != ppb_bytes or ppb_stride != lemma["derived_word_stores"]["stride_bytes"]:
        raise FormatError("OPEN selected PPB metadata stride/copy units are incoherent")
    if (arc[0xb8f8]["source_register"] != arc[0xb8dc]["destination_register"] or
            any(arc[0xb8f8][k] != arc[0xb984][k]
                for k in ("operation", "destination_register", "source_register", "operand_register")) or
            arc[0xbe6c]["source_register"] != arc[0xbe60]["destination_register"] or
            arc[0xbe6c]["operand_register"] != arc[0xbe58]["destination_register"]):
        raise FormatError("OPEN selected metadata delivery/release value linkage is incoherent")
    # The endpoint/index equations classify metadata; they do not borrow a
    # live allocation, validate queue capacity or certify a source plane.
    return {"basis": dict(fresh["basis"]), "validation": validation, "init_reply": init,
            "instruction_anchors": anchors, "elf_receipts": elf_receipts,
            "relocation_receipts": relocations, "selected_arc_edges": edges,
            "arm_path": {
                "identities": {"C": "ARM controller", "K": "ARM API host-channel record",
                               "H": "allocated ARM decoder/channel object", "D": "ARC channel context",
                               "F": "ARC record-pool source", "host_command_record": "valid incoming host request"},
                "selected_dispatch_call_site": 0x62d8, "dispatch_selector_and_argument_continuity_validated": False,
                "controller_root_global_offset": ad(0xa30c),
                "host_channel_record_stride_bytes": ai(0x908) * (1 << arm[0x918]["shift_amount"]),
                "host_channel_record_base_offset": ai(0xb0c),
                "ordinary_builder_requires_zero_selected_flag": True,
                "special_channel_bypass_validated": False,
                "builder": {"entry": 0x27480, "frame_bytes": frame,
                            "request_stack_offset": ai(0x274a0), "response_stack_offset": ai(0x274a4),
                            "packet_bytes": ai(0x274a8), "command": arm[0x274d0]["literal_value"],
                            "channel_reply_request_byte_offset": ad(0x274dc),
                            "bank_count_request_byte_offset": ad(0x2750c),
                            "bank_count_H_offset": ad(0xfb2c) + 4,
                            "timeout_argument": ai(0x27560), "transport_entry": arm[0x27578]["target_blob_file_offset"],
                            "saved_transport_status_stack_offset": ad(0x2757c),
                            "raw_reply_to_H": [{"reply_byte_offset": a, "H_offset": b} for a, b in raw_reply],
                            "writes_raw_outputs_before_transport_status_check": True,
                            "translation": {"reply_byte_offset": ad(0x27598), "map_C_offset": ad(0x275a4),
                                            "output_H_offset": ai(0x275a0), "helper_entry": arm[0x275ac]["target_blob_file_offset"],
                                            "store_precedes_bounds_check": init["arm_translations"]["helper_store_precedes_bounds_check"],
                                            "helper_status_checked": False, "stored_output_validated": False,
                                            "same_map_as_INIT_validated": False},
                            "unconditional_marker_H_offset": ad(0x275b4), "marker_value": ai(0x275b0),
                            "marker_proves_accepted_object": False, "returns_saved_transport_status": True},
                "publication": {"transport_status_branch": 0xfb80, "zero_status_success_target": arm[0xfb80]["target_blob_file_offset"],
                                "nonzero_status_cleanup_is_fallthrough": True, "failure_cleanup_call": 0xfb88,
                                "cleanup_callee": arm[0xfb88]["target_blob_file_offset"], "deallocation_semantics_validated": False,
                                "normal_outptr_store": 0xfbb0, "controller_table_store": 0xfbcc,
                                "controller_table_C_offset": ad(0xfbc8),
                                "failure_suppresses_normal_publication": True,
                                "postpublication_opaque_return_statuses_checked": False,
                                "forced_success_value": ai(0xfbe4)}},
            "outer_reply": {"conditional_on_selected_fresh_arc_execution": True,
                            "command": arm[0x274d0]["literal_value"], "status": am(0x24b08),
                            "reply_buffer_register": reply_register,
                            "word2": {"interpretation": "delivery_metadata_ring", "D_field_offset": ring_fields[0],
                                      "offset_from_F": ring_offsets_from_f[0], "offset_from_D": f_offset + ring_offsets_from_f[0]},
                            "word3": {"interpretation": "return_metadata_ring", "D_field_offset": ring_fields[1],
                                      "offset_from_F": ring_offsets_from_f[1], "offset_from_D": f_offset + ring_offsets_from_f[1]},
                            "word4": {"interpretation": "dm_return_info channel entry", "offset_from_B": rebased,
                                      "channel_stride_bytes": channel_stride, "channel_count": object_record["size"] // channel_stride,
                                      "word_count_per_channel": channel_stride // 4,
                                      "channel_scale_vendor_ISA_validated": False},
                            "bank_count_inclusive_max": count_max, "zero_bank_count_rejected": False,
                            "constructor_status_checked": False, "transport_checks_metadata_words": False,
                            "return_metadata_initialized_words": [av(0x247f0), 0],
                            "NOBITS_initialized_contents_proven": False},
            "record_pool": {"context_name": "D", "pool_name": "F", "F_offset_from_D": f_offset,
                            "source_D_field_offset": base_context + am(0x267b0),
                            "inherited_constructor_preservation": lemma,
                            "selected_bank_count_discharges_nonclobber_bound": count_max <= source_init["earlier_variable_loop"]["maximum_nonclobbering_count"],
                            "metadata_pool_D_field_offset": metadata_field,
                            "metadata_pool_offset_from_F": metadata_from_f,
                            "metadata_pool_offset_from_D": f_offset + metadata_from_f,
                            "allocation_extent_validated": False, "runtime_pool_identity_validated": False},
            "rings": {"header_word_count": av(0xb588) // 4, "header_bytes": av(0xb588),
                      "index_unit_bytes": 1 << av(0xb5e0), "index_inclusive_min": av(0xb598),
                      "index_inclusive_max": av(0xb5a8), "data_entry_count": av(0xb5a8) - av(0xb598) + 1,
                      "declared_index_extent_bytes": ring_bytes, "ring_base_gap_bytes": ring_bytes,
                      "allocation_extent_validated": False, "full_ring_protection_validated": False,
                      "invalid_put_can_fall_through_if_opaque_callees_return": True,
                      "get_empty_and_invalid_both_return_zero": True, "get_MMIO_completion_bounded": False,
                      "selected_delivery_payload": {"interpretation": "PPB metadata record address",
                                                    "record_stride_bytes": ppb_stride, "copy_bytes": ppb_bytes,
                                                    "record_count": lemma["derived_word_stores"]["count"],
                                                    "index_range_runtime_validated": False, "raw_source_plane": False},
                      "release_queue": {"slot_byte_gate_offset": am(0xbe44), "gate_skip_value": av(0xbe48),
                                        "published_return_info_word": 0, "destination_ring_D_field_offset": av(0xbe60),
                                        "destination_header_byte_offset": am(0xbe6c), "stored_index_validated": False}},
            "activation_snapshot": {"channel_table_local_base": table_base,
                                    "channel_stride_bytes": 1 << arc[0x9fa8]["shift_amount"],
                                    "copy_bytes": av(0x9fc0), "local_destination": activated_base,
                                    "delivery_ring_field_local_address": activated_delivery,
                                    "return_ring_field_local_address": activated_return,
                                    "metadata_pool_field_local_address": activated_pool,
                                    "successful_byte_preserving_copy_assumed": True,
                                    "same_channel_and_unchanged_entry_assumed": True,
                                    "runtime_active_context_validated": False},
            "assumptions": init["assumptions"] + [
                "Selected host entry 0x51c8 receives a valid incoming command record; the isolated dispatch call proves neither selector nor argument continuity.",
                "Successful opaque ARM configuration/allocation supplies valid disjoint C/K/H storage and reaches the ordinary r9=0 builder path; special-channel bypass and opaque setup/cleanup semantics are not validated.",
                "Selected ARC inputs have initialized state, channel 0..15, an empty channel slot and a valid normalized D; unsupported parameter-normalization and allocation edges remain premises.",
                "The inherited conditional constructor lemma supplies F=D+0x794 and preservation of D+0x530 under valid nonaliasing storage, unchanged source, base/no-wrap, count and callee-preservation conditions; it is not live pool identity.",
                "Original ARC call/local-literal edges survive unresolved relocation effects; legacy ARC fields, delay slots, STATUS-PC dispatch and vendor channel-shift semantics remain conditional.",
                "Selected nested ARC callees preserve the saved reply buffer and source operands. Successful DMA/copy completion, visibility, same-channel activation and unchanged table/fields are assumed, not runtime receipts.",
                "OPEN C+8 translation uses a valid stable map disjoint from H+0x58 and the protected stack; INIT's C+12 initialized map does not establish this map identity. Helper stores before bounds checking and its caller ignores status.",
                "The selected display metadata index is within the inherited 34-record pool; ring payload classification does not prove valid queues, raw-plane layout, ownership, a lease, or operational completion.",
                "An old event and nonzero mailbox can satisfy shared transport predicates when freshness is relaxed; this output inherits the INIT countermodel, not proof of a current OPEN acknowledgment."],
            "validation_scope": {"conditional_reply_metadata": True, "raw_source_plane_contract": False,
                                 "full_PPB_contract_invoked": False, "operational_queue_validity": False,
                                 "runtime_observed": False, "freshness_proven": False,
                                 "operational_coherence_proven": False, "allocation_lifetime_proven": False,
                                 "source_plane_lease": False, "active_decode_context": False,
                                 "hardware_aliasing_proven": False, "standalone_execution": False, "public_route": False}}


def _bootstrap_word(payload, offset):
    if offset % 4:
        raise FormatError("bootstrap word is not aligned")
    return struct.unpack("<I", bounded(payload, offset, 4, "bootstrap word"))[0]


def _a32_branch(payload, offset, link=False, condition=14):
    """Decode only audited A32 B/BL, not Thumb, BLX or arbitrary conditions."""
    word = _bootstrap_word(payload, offset)
    if condition not in (0, 1, 11, 12, 14) or word >> 24 != (condition << 4) | 10 | int(link):
        raise FormatError("bootstrap instruction is not the required A32 branch")
    displacement = word & 0xffffff
    if displacement & 0x800000:
        displacement -= 1 << 24
    target = offset + 8 + displacement * 4
    _bootstrap_word(payload, target)
    return {"blob_file_offset": offset, "word": word,
            "operation": "BL" if link else "B", "condition": condition,
            "target_blob_file_offset": target}


def _a32_branch_candidates_in_regions(payload, regions):
    """Decode A32 B/BL bit patterns only inside caller regions already pinned by hash."""
    total = sum(size for _, _, size in regions)
    if (len(regions) > MAX_CHANNEL_FIELD_CALLER_SCAN_REGIONS or
            total > MAX_CHANNEL_FIELD_CALLER_SCAN_BYTES):
        raise FormatError("channel-field caller scan budget exceeded")
    candidates = []
    for role, start, size in regions:
        if start % 4 or size <= 0 or size % 4:
            raise FormatError("channel-field caller scan region is not aligned")
        data = bounded(payload, start, size, "channel-field caller scan region")
        for relative in range(0, size, 4):
            word = struct.unpack_from("<I", data, relative)[0]
            condition = word >> 28
            # cond=0xf uses the unconditional/BLX space, not this A32 B/BL form.
            if condition == 15 or word & 0x0e000000 != 0x0a000000:
                continue
            displacement = word & 0xffffff
            if displacement & 0x800000:
                displacement -= 1 << 24
            offset = start + relative
            target = offset + 8 + displacement * 4
            candidates.append({
                "region_role": role, "blob_file_offset": offset, "word": word,
                "operation": "BL" if word & 0x01000000 else "B",
                "condition": condition, "target_blob_file_offset": target,
                "target_inside_payload": 0 <= target <= len(payload) - 4,
            })
    return candidates


def _a32_literal(payload, offset):
    """Decode AL LDR word [PC, +/-imm12], with no writeback or register offset."""
    word = _bootstrap_word(payload, offset)
    if word & 0xff7f0000 != 0xe51f0000:
        raise FormatError("bootstrap instruction is not an AL A32 LDR literal")
    displacement = word & 0xfff
    literal = offset + 8 + (displacement if word & (1 << 23) else -displacement)
    value = _bootstrap_word(payload, literal)
    return {"blob_file_offset": offset, "word": word, "operation": "LDR literal",
            "destination_register": (word >> 12) & 15,
            "literal_blob_file_offset": literal, "literal_value": value}


def _bootstrap_map(payload, images):
    """Private pure validator; public callers must pin SHA/size before entering."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("bootstrap payload size does not match the bundled baseline")
    extents = [(0x2ea60, 0x79dd8), (0x79dd8, 0xcfbb0)]
    if [(image["blob_file_offset"], image["blob_file_end"]) for image in images] != extents:
        raise FormatError("bootstrap catalog does not match the parsed ELF identities")
    anchors = []

    def word(offset, expected):
        actual = _bootstrap_word(payload, offset)
        if actual != expected:
            raise FormatError(f"bootstrap word at {offset:#x} does not match the baseline")
        anchors.append({"blob_file_offset": offset, "word": actual,
                        "operation": "validated word"})
        return actual

    def branch(offset, target, link=False, condition=14):
        record = _a32_branch(payload, offset, link, condition)
        if record["target_blob_file_offset"] != target:
            raise FormatError("bootstrap branch target does not match the baseline")
        anchors.append(record)

    def literal(offset, position, value, register):
        record = _a32_literal(payload, offset)
        if (record["literal_blob_file_offset"], record["literal_value"],
                record["destination_register"]) != (position, value, register):
            raise FormatError("bootstrap literal does not match the baseline")
        anchors.append(record)

    # Flat ARM vectors + coherent reset/main branches corroborate ARMCR4 in
    # crystalhd_fleafuncs.c:1353-1355. Do not decode either embedded ARC image.
    for offset, position, target in ((0, 0x20, 0x2ca00), (4, 0x24, 0x3c),
                                     (8, 0x28, 0x5c), (12, 0x2c, 0x7c),
                                     (16, 0x30, 0x9c), (24, 0x34, 0xdc),
                                     (28, 0x38, 0x104)):
        literal(offset, position, target, 15)
        _bootstrap_word(payload, target)
    word(0x2ca00, 0xee100f31)
    branch(0x2cc0c, 0x74bc, link=True)
    word(0x74bc, 0xe92d4010)
    branch(0x7530, 0x8d98)
    word(0x8d98, 0xe92d4ff8)
    # Host ARM1 mailbox read, queued receive and actual dispatcher call.
    word(0x8d48, 0xe92d4010)
    literal(0x8d54, 0x8f74, 0x100e0000, 0)
    word(0x8d58, 0xe590401c)
    word(0x8d70, 0xe1a00004)
    branch(0x8d74, 0x8cf4, link=True)
    word(0x8cf4, 0xe92d4038)
    word(0x8d04, 0xe3001100)
    branch(0x8d14, 0x8c30, link=True)
    word(0x8c30, 0xe92d47f0)
    branch(0x8e40, 0x9048, link=True)
    word(0x9048, 0xe92d4070)
    branch(0x9204, 0x5f2c, link=True)
    # Compare-tree arithmetic: 0x73763108 - 7 - 0xfd = GET_VERSION.
    literal(0x5f78, 0x6174, 0x73763108, 2)
    word(0x5f8c, 0xe2420007)
    word(0x5fa0, 0xe24020fd)
    word(0x5fa4, 0xe1510002)
    branch(0x5fac, 0x6264, condition=0)
    branch(0x6270, 0x5ba4, link=True)
    word(0x5f5c, 0xe5950000)
    word(0x5f60, 0xe5860000)
    for offset, expected in ((0x5c10, 0xe3a00000), (0x5c14, 0xe5840008),
                             (0x5c18, 0xe5950004), (0x5c1c, 0xe5840004)):
        word(offset, expected)
    # Function entry/common-return words, not claimed complete function maps.
    functions = []
    for name, entry, prologue, exit_offset, epilogue in (
            ("host_command_dispatch", 0x5f2c, 0xe92d4070, 0x60c4, 0xe8bd8070),
            ("get_version", 0x5ba4, 0xe92d4070, 0x5c24, 0xe8bd8070),
            ("image_container_open", 0x26e7c, 0xe92d41f0, 0x26ebc, 0xe8bd81f0),
            ("image_container_read", 0x26f74, 0xe92d41f0, 0x26fb8, 0xe8bd81f0),
            ("image_container_close", 0x26fdc, 0xe92d4010, 0x26ff4, 0xe8bd8010)):
        word(entry, prologue)
        word(exit_offset, epilogue)
        functions.append({"name": name, "entry_blob_file_offset": entry,
                          "common_exit_blob_file_offset": exit_offset})
    # OPEN selection in the same dispatcher: command - GET_VERSION == 0xfc.
    # Record buffers are 256 bytes; this is a host selector policy, not codec
    # capability or proof that an admitted selector can decode a bitstream.
    for offset, expected in ((0x5f44, 0xe2845014), (0x5f48, 0xe2846f45),
                             (0x5f4c, 0xe3002100), (0x5f50, 0xe3a01000),
                             (0x5f54, 0xe1a00006), (0x5f70, 0xe5951000),
                             (0x5f7c, 0xe1510002), (0x5f80, 0xe0410002),
                             (0x5f90, 0xe1510000), (0x5f94, 0xe0412000),
                             (0x5fa8, 0xe0410002), (0x5fe8, 0xe3500001),
                             (0x5ff0, 0xe3500002), (0x5ff8, 0xe35000fc),
                             (0x62d4, 0xe1a00004)):
        word(offset, expected)
    for offset, target, condition in ((0x5f84, 0x662c, 0), (0x5f88, 0x602c, 12),
                                      (0x5f98, 0x6428, 0), (0x5f9c, 0x6004, 12),
                                      (0x5fb0, 0x5fe8, 12), (0x5fec, 0x628c, 0),
                                      (0x5ff4, 0x62ac, 0), (0x5ffc, 0x60b8, 1)):
        branch(offset, target, condition=condition)
    branch(0x5f58, 0x206e4, link=True)
    branch(0x6000, 0x62cc)
    branch(0x62d8, 0x51c8, link=True)
    # Initial-state and free-slot checks precede the low-byte selector ladder.
    for offset, expected in ((0x51c8, 0xe92d4ff0), (0x51cc, 0xe24dd01c),
                             (0x51d0, 0xe3e07000), (0x51d4, 0xe3a05000),
                             (0x51d8, 0xe3500000), (0x51e0, 0xe2806014),
                             (0x51e4, 0xe2804f45), (0x51ec, 0xe3e08000),
                             (0x51f0, 0xe59b0000), (0x51f4, 0xe3500001),
                             (0x523c, 0xe59b0004), (0x5240, 0xe3a01073),
                             (0x5244, 0xe0010195), (0x5248, 0xe0801101),
                             (0x524c, 0xe5d110c4), (0x5250, 0xe3510000),
                             (0x5258, 0xe2855001), (0x525c, 0xe3550004),
                             (0x5264, 0xe3570000), (0x5300, 0xe1a07005),
                             (0x5218, 0xe28dd01c), (0x521c, 0xe8bd8ff0)):
        word(offset, expected)
    literal(0x51e8, 0x5128, 0xd1ff4, 11)
    branch(0x51dc, 0x5220, condition=0)
    branch(0x51f8, 0x5234, condition=0)
    branch(0x5254, 0x52f4, condition=0)
    branch(0x5260, 0x5240, condition=11)
    branch(0x5268, 0x59f0, condition=11)
    branch(0x5304, 0x5264)
    # LDRB deliberately establishes no validation of algorithm word high bits.
    word(0x5294, 0xe5d60024)
    word(0x52c4, 0xe3a01008)
    routes = []
    for selector, compare, jump, target in ((1, 0x52a0, 0x52a4, 0x5408),
                                            (0, 0x52a8, 0x52ac, 0x5420),
                                            (4, 0x52b0, 0x52b4, 0x5438),
                                            (7, 0x52b8, 0x52bc, 0x5454),
                                            (6, 0x52c0, 0x52c8, 0x547c),
                                            (8, 0x52cc, 0x52d0, 0x55b0)):
        word(compare, 0xe3500000 | selector)
        branch(jump, target, condition=0)
        routes.append({"low_byte_selector": selector, "compare_blob_file_offset": compare,
                       "branch_blob_file_offset": jump, "target_blob_file_offset": target})
    for offset, expected in ((0x52d4, 0xe28f00b8), (0x52dc, 0xe584800c),
                             (0x52e0, 0xe5960004), (0x52e4, 0xe5840004),
                             (0x52e8, 0xe5848008), (0x52ec, 0xe3a00002)):
        word(offset, expected)
    branch(0x52d8, 0x203c4, link=True)
    branch(0x52f0, 0x5218)
    open_policy = {
        "command": 0x73763100, "dispatcher_call_blob_file_offset": 0x62d8,
        "entry_blob_file_offset": 0x51c8, "record_buffer_bytes": 256,
        "request_record_offset": 0x14, "reply_record_offset": 0x114,
        "algorithm_word_index": 9, "algorithm_load_blob_file_offset": 0x5294,
        "algorithm_bits_compared": 8, "algorithm_upper_bits_checked": False,
        "comparison_routes": routes,
        "named_rejected_selectors": [
            {"name": name, "value": value, "source": f"include/7411d.h:{line}"}
            for name, value, line in (("H261", 2, 390), ("H263", 3, 391), ("MPEG1", 5, 393))],
        "fallback": {"entry_blob_file_offset": 0x52d4, "channel_id_word_index": 3,
                     "channel_id": 0xffffffff, "status_word_index": 2, "status": 0xffffffff,
                     "sequence_copy_blob_file_offsets": [0x52e0, 0x52e4],
                     "internal_return": 2, "common_exit_blob_file_offset": 0x5218},
        "preconditions": {"state_word_equals": 1, "state_check_blob_file_offset": 0x51f4,
                          "free_channel_slot_required": True, "slot_scan_limit": 4,
                          "slot_check_blob_file_offset": 0x5250, "device_observed": False},
        "device_observed": False,
        "scope": "Static host OPEN selector policy, not channel-open or codec capability proof."}
    # DeviceStart passes host-interface +4 as an out-pointer to API init. Init
    # writes the same static context that the START getter returns: matching
    # slot offsets alone would not establish the OPEN -> START cache handoff.
    for offset, expected in ((0x54c, 0xe92d40f0), (0x558, 0xe1a07001),
                             (0x880, 0xe5874000), (0x89c, 0xe12fff1e),
                             (0x526c, 0xe3a00073), (0x5278, 0xe0050097),
                             (0x5298, 0xe3a0a001), (0x529c, 0xe3a09000)):
        word(offset, expected)
    literal(0x5ce8, 0x5128, 0xd1ff4, 7)
    literal(0x5d44, 0x5ed4, 0xd1ff8, 1)
    branch(0x5d4c, 0x54c, link=True)
    literal(0x56c, 0x6fc, 0xd3a00, 4)
    literal(0x898, 0x6fc, 0xd3a00, 0)
    # Each admitted OPEN branch writes a byte into context + slot*0x1cc +0xd0.
    for offset, expected in ((0x5408, 0xe59b0004), (0x540c, 0xe0800105),
                             (0x5410, 0xe5c0a0d0), (0x5420, 0xe59b0004),
                             (0x5424, 0xe0800105), (0x5428, 0xe5c090d0),
                             (0x5438, 0xe59b1004), (0x543c, 0xe3a00004),
                             (0x5440, 0xe0811105), (0x5444, 0xe5c100d0),
                             (0x5454, 0xe59b1004), (0x5458, 0xe3a00007),
                             (0x545c, 0xe0811105), (0x5460, 0xe5c100d0),
                             (0x547c, 0xe59b0004), (0x5480, 0xe0800105),
                             (0x5484, 0xe5c010d0), (0x55b0, 0xe59b0004),
                             (0x55b4, 0xe0800105), (0x55b8, 0xe5c010d0)):
        word(offset, expected)
    for offset in (0x541c, 0x5434, 0x5450, 0x5478, 0x549c):
        branch(offset, 0x55d4)
    # START command - dispatcher base == 0x12. The handler indexes the supplied
    # channel before its opened-byte check; this does not prove range safety.
    for offset, expected in ((0x602c, 0xe350001c), (0x6038, 0xe3500015),
                             (0x604c, 0xe3500012), (0x6684, 0xe1a00004),
                             (0x4630, 0xe92d47ff), (0x463c, 0xe2805014),
                             (0x4640, 0xe2804f45), (0x4644, 0xe5957008),
                             (0x4660, 0xe3a00073), (0x4668, 0xe0060097),
                             (0x466c, 0xe59a0004), (0x4670, 0xe0802106),
                             (0x4674, 0xe5d200c4), (0x4678, 0xe3500001),
                             (0x4830, 0xe1a00007)):
        word(offset, expected)
    branch(0x6034, 0x607c, condition=12)
    branch(0x6040, 0x6060, condition=12)
    branch(0x6050, 0x667c, condition=0)
    branch(0x6688, 0x4630, link=True)
    literal(0x465c, 0x3de8, 0xd1ff4, 10)
    branch(0x467c, 0x4808, condition=0)
    branch(0x4834, 0xdf0, link=True)
    # API START obtains that context and replaces the first configuration byte
    # from the cached OPEN byte. The two configuration copies are 56 bytes.
    for offset, expected in ((0xdf8, 0xe1a01000), (0xe08, 0xe3a02073),
                             (0xe0c, 0xe0010291), (0xe10, 0xe0807101),
                             (0xe14, 0xe2874018), (0xff0, 0xe5940008),
                             (0xff4, 0xe28d1004), (0x1054, 0xe5d720d0),
                             (0x1058, 0xe5cd2004), (0x10ac, 0xe5940008),
                             (0x10b0, 0xe28d1004), (0xee9c, 0xe1a04001),
                             (0xeecc, 0xe3a02038), (0xeed4, 0xe1a00004),
                             (0xf0cc, 0xe92d4ffe), (0xf0d0, 0xe1a06000),
                             (0xf0d4, 0xe1a0b001), (0xf104, 0xe1a04006),
                             (0xf13c, 0xe3a02038), (0xf140, 0xe1a0100b),
                             (0xf144, 0xe2840068), (0xf1d4, 0xe5d40068),
                             (0xf1d8, 0xe3500007), (0xf1e0, 0xe3a00004),
                             (0xf1e4, 0xe5c40068), (0xf390, 0xe5d42068),
                             (0xf394, 0xe1a03005), (0xf398, 0xe1a00007),
                             (0xf39c, 0xe5961000)):
        word(offset, expected)
    branch(0xdfc, 0x898, link=True)
    branch(0xff8, 0xee94, link=True)
    literal(0xeed0, 0xf6c8, 0x2dd88, 1)
    bounded(payload, 0x2dd88, 56, "START default configuration")
    branch(0xeed8, 0x20708, link=True)
    branch(0x10b4, 0xf0cc, link=True)
    branch(0xf148, 0x20708, link=True)
    branch(0xf1dc, 0xf1e8, condition=1)
    branch(0xf3a0, 0x276b0, link=True)
    branch(0xf3a4, 0x206a0, link=True)
    # The packet helper preserves r2 as word1 and returns the transport result.
    # Its caller immediately makes another BL without checking that result.
    for offset, expected in ((0x276b0, 0xe92d4ff0), (0x276c0, 0xe1a09002),
                             (0x276d4, 0xe28d4f42), (0x276fc, 0xe1a05004),
                             (0x27708, 0xe5850000), (0x2770c, 0xe5859004),
                             (0x27730, 0xe1a02004), (0x27740, 0xe58d0008),
                             (0x27744, 0xe59d0008), (0x27748, 0xe28ddf81),
                             (0x2774c, 0xe8bd8ff0), (0x27068, 0xe1a06002),
                             (0x27080, 0xe5d4008c), (0x27084, 0xe3500000),
                             (0x270ac, 0xe3a020fc), (0x270b0, 0xe1a01006),
                             (0x270b4, 0xe5940094), (0x270bc, 0xe5941118),
                             (0x270c0, 0xe59421cc), (0x270c4, 0xe1a00004),
                             (0x25024, 0xe5903004), (0x25028, 0xe5933000),
                             (0x2502c, 0xe7832001), (0x270d8, 0xe1a0100b),
                             (0x270dc, 0xe5940088)):
        word(offset, expected)
    literal(0x27704, 0x27aac, 0x73760005, 0)
    branch(0x2773c, 0x2705c, link=True)
    branch(0x27088, 0x27098, condition=0)
    branch(0x270b8, 0x20708, link=True)
    branch(0x270c8, 0x25024, link=True)
    branch(0x270e0, 0x20598, link=True)
    decoder_start = {
        "command": 0x7376311a, "command_source": "include/7411d.h:173",
        "dispatcher_call_blob_file_offset": 0x6688, "entry_blob_file_offset": 0x4630,
        "request_channel_word_index": 2, "request_channel_load_blob_file_offset": 0x4644,
        "context_link": {"host_interface_global_address": 0xd1ff4,
                         "init_output_pointer_address": 0xd1ff8,
                         "context_address": 0xd3a00, "slot_stride_bytes": 0x1cc,
                         "cached_algorithm_byte_offset": 0xd0,
                         "instruction_blob_file_offsets": [0x5ce8, 0x5d44, 0x5d4c,
                                                           0x558, 0x56c, 0x880, 0x898]},
        "preconditions": {"opened_byte_equals": 1, "opened_byte_offset": 0xc4,
                          "check_blob_file_offset": 0x4678,
                          "channel_range_validation_established": False,
                          "device_observed": False},
        "selector_mapping": [
            {"host_open_low_byte": selector, "cached_algorithm_byte": cached,
             "inner_start_algorithm_byte": inner, "cache_store_blob_file_offset": store}
            for selector, cached, inner, store in ((0, 0, 0, 0x5428), (1, 1, 1, 0x5410),
                                                   (4, 4, 4, 0x5444), (6, 8, 8, 0x5484),
                                                   (7, 7, 4, 0x5460), (8, 8, 8, 0x55b8))],
        "configuration": {"bytes": 56, "cache_load_blob_file_offset": 0x1054,
                          "first_byte_store_blob_file_offset": 0x1058,
                          "copy_call_blob_file_offsets": [0xeed8, 0xf148],
                          "channel_configuration_offset": 0x68,
                          "normalize_compare_blob_file_offset": 0xf1d8,
                          "normalize_store_blob_file_offset": 0xf1e4},
        "inner_packet": {"command": 0x73760005, "command_word_index": 0,
                         "algorithm_word_index": 1, "entry_blob_file_offset": 0x276b0,
                         "algorithm_store_blob_file_offset": 0x2770c,
                         "transport_call_blob_file_offset": 0x2773c,
                         "copy_bytes": 252, "copy_call_blob_file_offset": 0x270b8,
                         "register_write_call_blob_file_offset": 0x270c8,
                         "wait_call_blob_file_offset": 0x270e0,
                         "publication_is_path_dependent": True,
                         "return_checked_by_caller": False,
                         "unchecked_continuation_blob_file_offset": 0xf3a4},
        "device_observed": False,
        "scope": "Static cached-selector path, not inner decoder acceptance or codec capability proof.",
        "limitations": ["The specific packet helper return is unchecked on this continuation; earlier START state/configuration failures can still be returned.",
                        "Numeric inner algorithm bytes are not RAVE parser protocol enums or decoded ARC calls.",
                        "AVS and MVC reachability through the host API is not established."]}
    # ARM initialized catalog data is outside both ELF files, inside payload.
    root = _bootstrap_word(payload, 0xcfc00)
    if root != 0xcfbe8:
        raise FormatError("bootstrap catalog root does not match the baseline")
    callbacks = [_bootstrap_word(payload, 0xcfcf0 + index * 4) for index in range(3)]
    if callbacks != [0x26e7c, 0x26f74, 0x26fdc]:
        raise FormatError("bootstrap catalog callbacks do not match the baseline")
    descriptors = []
    slots = [_bootstrap_word(payload, root + index * 4) for index in range(6)]
    if slots != [0xcfbd0, 0xcfbdc, 0, 0, 0, 0]:
        raise FormatError("bootstrap catalog slots do not match the baseline")
    for index, (start, end) in enumerate(extents):
        descriptor = slots[index]
        size_pointer, blob_pointer, metadata_pointer = (
            _bootstrap_word(payload, descriptor + offset) for offset in (0, 4, 8))
        size = _bootstrap_word(payload, size_pointer)
        metadata = _bootstrap_word(payload, metadata_pointer)
        expected_pointers = ((0xcfbb0, start, 0xcfbcc), (0xcfbb4, start, 0xcfbb8))[index]
        if (size_pointer, blob_pointer, metadata_pointer) != expected_pointers:
            raise FormatError("bootstrap image descriptor pointers do not match the baseline")
        if size != end - start:
            raise FormatError("bootstrap declared image size differs from its ELF extent")
        if metadata != (0, 0x90000)[index]:
            raise FormatError("bootstrap catalog metadata word does not match the baseline")
        bounded(payload, blob_pointer, size, "bootstrap catalog image")
        descriptors.append({"slot": index, "descriptor_blob_file_offset": descriptor,
                            "size_pointer_blob_file_offset": size_pointer,
                            "image_blob_file_offset": start, "image_blob_file_end": end,
                            "declared_image_size": size,
                            "metadata_pointer_blob_file_offset": metadata_pointer,
                            "metadata_word": metadata})
    # Joint static derivation, not a hardware observation: driver programs
    # BORCH_END = payload size - 1; firmware reply uses that register + 0x201.
    literal(0x9298, 0x8be8, 0x100f6000, 0)
    word(0x929c, 0xe5900004)
    word(0x92a0, 0xe3001201)
    word(0x92a4, 0xe0804001)
    if len(anchors) > MAX_BOOTSTRAP_ANCHORS:
        raise FormatError("bootstrap instruction-anchor budget exceeded")
    scrub_end = len(payload) - 1
    return {"schema_version": 1, "isa": "A32", "endianness": "little",
            "regions": [{"blob_file_offset": start, "blob_file_end": end, "kind": kind}
                        for start, end, kind in (
                            (0, extents[0][0], "ARM bootstrap code, literals and data"),
                            (*extents[0], "embedded ARC ELF image 0"),
                            (*extents[1], "embedded ARC ELF image 1"),
                            (extents[1][1], len(payload), "ARM initialized data and padding"))],
            "cmac": {"length_slot_blob_file_offset": len(payload), "length": 16,
                     "blob_file_offset": len(payload) + 4, "authentication_verified": False},
            "image_catalog": {"root_blob_file_offset": 0xcfc00,
                              "table_blob_file_offset": root, "slot_count": 6,
                              "descriptor_blob_file_offsets": slots,
                              "callback_table_blob_file_offset": 0xcfcf0,
                              "callback_entry_blob_file_offsets": callbacks,
                              "images": descriptors},
            "instruction_anchors": anchors, "function_anchors": functions,
            "host_channel_open_policy": open_policy,
            "host_decoder_start": decoder_start,
            "host_mailbox_dispatch": {"arm_mailbox_address": 0x100e001c,
                                      "receive_entry_blob_file_offset": 0x8d48,
                                      "queue_entry_blob_file_offset": 0x8cf4,
                                      "dispatch_entry_blob_file_offset": 0x5f2c,
                                      "get_version_command": 0x73763004,
                                      "get_version_entry_blob_file_offset": 0x5ba4},
            "static_derived_layout": {
                "device_observed": False,
                "scrub_end": {"address": scrub_end, "source_kind": "driver",
                              "source": "driver/linux/FleaDefs.h:42"},
                "host_command": {"address": scrub_end + 1 + 0x100, "source_kind": "driver",
                                 "source": "driver/linux/FleaDefs.h:51; driver/linux/crystalhd_fleafuncs.c:1288"},
                "reply": {"address": scrub_end + 0x201, "source_kind": "driver and ARM anchors",
                          "instruction_blob_file_offsets": [0x9298, 0x929c, 0x92a0, 0x92a4]}},
            "limitations": ["Only fixed baseline instruction anchors are decoded; this is not a complete ARM call graph.",
                            "Region boundaries do not classify every byte as code or data; ARM also uses Thumb helpers.",
                            "Catalog metadata words and OL/IL physical destinations are not established.",
                            "SHA-256 identity is not CMAC authentication or runtime capability proof."]}


def _csc_command_map(payload, images):
    """Private local-path validator; the public entry pins bundled SHA and size."""
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            [(i["blob_file_offset"], i["blob_file_end"]) for i in images] !=
            [(0x2ea60, 0x79dd8), (0x79dd8, 0xcfbb0)]):
        raise FormatError("CSC command image identities do not match the baseline")
    fixed = ((0x5f2c, 0xe92d4070), (0x5f30, 0xe1a04000), (0x5f3c, 0xe3540000),
             (0x5f40, 0x0a000023), (0x5f44, 0xe2845014),
             (0x5f70, 0xe5951000), (0x5f74, 0xe3a05008), (0x5f78, 0xe59f21f4),
             (0x5f7c, 0xe1510002), (0x5f80, 0xe0410002), (0x5f84, 0x0a0001a8),
             (0x5f88, 0xca000027), (0x602c, 0xe350001c), (0x6030, 0x0a000219),
             (0x6034, 0xca000010), (0x607c, 0xe3500089), (0x6080, 0x0a000227),
             (0x6084, 0xca000006), (0x6088, 0xe350002e), (0x608c, 0x0a00020c),
             (0x6090, 0xe350003c), (0x6094, 0x0a000212), (0x6098, 0xe3500088),
             (0x609c, 0x1a000005), (0x60b8, 0xe28f00e8), (0x60bc, 0xeb0068c0),
             (0x60c0, 0xe5c45010), (0x60c4, 0xe8bd8070))
    if len(fixed) > MAX_CSC_COMMAND_ANCHORS:
        raise FormatError("CSC command instruction-anchor budget exceeded")
    anchors = {}
    # Validate every word before decoding any branch, including skipped arms.
    for offset, expected in fixed:
        actual = _bootstrap_word(payload, offset)
        if actual != expected:
            raise FormatError(f"CSC command word at {offset:#x} does not match the baseline")
        anchors[offset] = {"blob_file_offset": offset, "word": actual,
                           "operation": "validated word"}
    literal = _a32_literal(payload, 0x5f78)
    if (literal["literal_blob_file_offset"], literal["literal_value"],
            literal["destination_register"]) != (0x6174, 0x73763108, 2):
        raise FormatError("CSC command base literal does not match the baseline")
    diagnostic = b"[fw] SMP_CmdApi_ProcessHstCmd(): Unknown Command\n\0"
    diagnostic_offset = 0x60b8 + 8 + (anchors[0x60b8]["word"] & 0xff)
    if bounded(payload, diagnostic_offset, len(diagnostic), "CSC command diagnostic") != diagnostic:
        raise FormatError("CSC command diagnostic does not match the baseline")
    anchors[0x5f78] = literal
    for offset, target, condition, link in (
            (0x5f40, 0x5fd4, 0, False), (0x5f84, 0x662c, 0, False),
            (0x5f88, 0x602c, 12, False), (0x6030, 0x689c, 0, False),
            (0x6034, 0x607c, 12, False), (0x6080, 0x6924, 0, False),
            (0x6084, 0x60a4, 12, False), (0x608c, 0x68c4, 0, False),
            (0x6094, 0x68e4, 0, False), (0x609c, 0x60b8, 1, False),
            (0x60bc, 0x203c4, 14, True)):
        record = _a32_branch(payload, offset, link, condition)
        if record["target_blob_file_offset"] != target:
            raise FormatError("CSC command branch does not match the baseline")
        anchors[offset] = record
    command = 0x73763180  # include/7411d.h:128,218; not a configurable probe.
    delta = command - literal["literal_value"]
    return {"isa": "A32", "endianness": "little",
            "command": {"name": "eCMD_C011_DEC_CHAN_SET_CSC", "value": command,
                        "source": "include/7411d.h:128,218"},
            "handler_entry_blob_file_offset": 0x5f2c,
            "record_command_byte_offset": anchors[0x5f44]["word"] & 0xff,
            "command_load_blob_file_offset": 0x5f70,
            "delta": delta, "subtract_blob_file_offset": 0x5f80,
            "subtract_updates_flags": bool(anchors[0x5f80]["word"] & (1 << 20)),
            "selected_path_comparisons": [
                {"compare_blob_file_offset": 0x5f7c, "relation": "signed greater than",
                 "lhs": command, "rhs": literal["literal_value"], "branch_blob_file_offset": 0x5f88},
                {"compare_blob_file_offset": 0x602c, "relation": "signed greater than",
                 "lhs": delta, "rhs": 0x1c, "branch_blob_file_offset": 0x6034},
                {"compare_blob_file_offset": 0x607c, "relation": "signed less than",
                 "lhs": delta, "rhs": 0x89, "not_taken_branch_blob_file_offset": 0x6084},
                {"compare_blob_file_offset": 0x6088, "relation": "not equal", "lhs": delta, "rhs": 0x2e},
                {"compare_blob_file_offset": 0x6090, "relation": "not equal", "lhs": delta, "rhs": 0x3c},
                {"compare_blob_file_offset": 0x6098, "relation": "not equal",
                 "lhs": delta, "rhs": 0x88, "branch_blob_file_offset": 0x609c}],
            "local_fallback": {
                "entry_blob_file_offset": 0x60b8,
                "diagnostic": {"blob_file_offset": diagnostic_offset,
                               "text": diagnostic[:-1].decode("ascii"), "nul_terminated": True},
                "logging_call_blob_file_offset": 0x60bc, "logging_callee_blob_file_offset": 0x203c4,
                "logging_callee_body_validated": False,
                "value_register_setup": {"blob_file_offset": 0x5f74, "register": 5, "value": 8},
                "record_byte_store": {"blob_file_offset": 0x60c0, "base_register": 4,
                                      "source_register": 5, "byte_offset": 0x10},
                "return_blob_file_offset": 0x60c4},
            "register_flow_assumption": "Unvalidated calls return and preserve callee-saved r4/r5; value propagation is conditional.",
            "library_context": {
                "wrapper_source": "linux_lib/libcrystalhd/libcrystalhd_if.cpp:3045-3050",
                "packing_source": "linux_lib/libcrystalhd/libcrystalhd_int_if.cpp:193-212",
                "register_symbol": "MISC2_GLOBAL_CTRL", "packing_selection": ["YUY2", "UYVY"],
                "kind": "source-derived packed-YUV422 selection, not a firmware matrix route"},
            "instruction_anchor_count": len(anchors), "instruction_anchors": list(anchors.values()),
            "limitations": [
                "Only this local instruction path is validated, under a non-null-record and returning-callee assumption.",
                "Branch target bodies outside the listed anchors, including the logging callee, are unvalidated.",
                "No device execution was observed; this is not a standalone execution proof or silicon capability claim."]}


def _stock_host_dispatch_domains(payload):
    """Partition all u32 commands through this fixed stock A32 selector only."""
    mask = 0xffffffff

    def normalize(parts):
        result = []
        for low, high in sorted(parts):
            if not 0 <= low <= high <= mask:
                raise FormatError("stock command interval is outside u32")
            if result and low <= result[-1][1] + 1:
                result[-1] = (result[-1][0], max(high, result[-1][1]))
            else:
                result.append((low, high))
        if len(result) > MAX_STOCK_HOST_COMMAND_PARTITIONS:
            raise FormatError("stock command partition budget exceeded")
        return result

    def intersect(left, right):
        return normalize((max(a, c), min(b, d)) for a, b in left for c, d in right
                         if max(a, c) <= min(b, d))

    def subtract(left, right):
        result = list(left)
        for c, d in right:
            remaining = []
            for a, b in result:
                if d < a or b < c:
                    remaining.append((a, b))
                else:
                    if a < c:
                        remaining.append((a, c - 1))
                    if d < b:
                        remaining.append((d + 1, b))
            result = remaining
        return normalize(result)

    def inverse(parts, value):
        coefficient, bias = value
        if coefficient == 0:
            return [(0, mask)] if any(a <= bias <= b for a, b in parts) else []
        result = []
        for a, b in parts:
            low, high = (a - bias) & mask, (b - bias) & mask
            result.extend([(low, high)] if low <= high else [(0, high), (low, mask)])
        return normalize(result)

    def predicate(flags, condition):
        if condition == 14:
            return [(0, mask)]
        if flags is None:
            raise FormatError("stock command branch has unknown flags")
        operation, left, right = flags
        if operation == "zero":
            if condition not in (0, 1):
                raise FormatError("stock command ADD flags used outside audited Z test")
            equal = inverse([(0, 0)], left)
            return equal if condition == 0 else subtract([(0, mask)], equal)
        if right[0] != 0:
            raise FormatError("stock command comparison has a variable RHS")
        rhs = right[1]
        if condition in (0, 1):
            equal = inverse([(rhs, rhs)], left)
            return equal if condition == 0 else subtract([(0, mask)], equal)
        if condition == 3:  # CC: CMP carry is clear exactly when unsigned lhs < rhs.
            return inverse([(0, rhs - 1)] if rhs else [], left)
        if condition == 12:  # GT: !Z && N == V, including signed subtraction overflow.
            signed = rhs if rhs < 0x80000000 else rhs - (1 << 32)
            values = ([(signed + 1, 0x7fffffff)] if 0 <= signed < 0x7fffffff else
                      [] if signed == 0x7fffffff else
                      [(0, 0x7fffffff)] if signed == -1 else
                      [(0, 0x7fffffff), ((signed + 1) & mask, mask)])
            return inverse(values, left)
        raise FormatError("unsupported stock command condition")

    def operand(word, registers):
        if word & (1 << 25):
            value, rotate = word & 0xff, ((word >> 8) & 15) * 2
            return (0, ((value >> rotate) | (value << ((32 - rotate) % 32))) & mask)
        if word & (1 << 4):
            raise FormatError("unsupported stock command register shift")
        value = registers[word & 15]
        if value is None:
            raise FormatError("stock command operand is unknown")
        shift, kind = (word >> 7) & 31, (word >> 5) & 3
        if kind == 0 and shift == 0:
            return value
        if kind == 2 and shift == 29 and value[0] == 0:
            signed = value[1] if value[1] < 0x80000000 else value[1] - (1 << 32)
            return (0, (signed >> 29) & mask)
        raise FormatError("unsupported stock command shifted operand")

    def affine(left, right, subtract_right=False):
        if left is None or right is None:
            raise FormatError("stock command arithmetic has unknown input")
        coefficient = left[0] - right[0] if subtract_right else left[0] + right[0]
        if coefficient not in (0, 1):
            raise FormatError("unsupported stock command affine expression")
        return (coefficient, (left[1] - right[1] if subtract_right else left[1] + right[1]) & mask)

    registers = [None] * 16
    registers[1] = (1, 0)
    queue = [(0x5f78, tuple(registers), None, [(0, mask)])]
    terminals, states, table_domains = {}, 0, {}
    while queue:
        pc, saved, flags, domain = queue.pop()
        if not domain:
            continue
        states += 1
        if states > MAX_STOCK_HOST_COMMAND_CFG_STATES:
            raise FormatError("stock command CFG state budget exceeded")
        if pc == 0x60b8 or 0x60c8 <= pc < 0x6984:
            terminals[pc] = normalize(terminals.get(pc, []) + domain)
            continue
        if not 0x5f78 <= pc < 0x60b8 or pc % 4:
            raise FormatError("stock command selector escaped its fixed CFG")
        word = _bootstrap_word(payload, pc)
        registers = list(saved)
        if pc == 0x6008:
            if word != 0x308ff102 or registers[2] is None:
                raise FormatError("unsupported stock command bounded table")
            allowed = predicate(flags, 3)
            taken = intersect(domain, allowed)
            queue.append((pc + 4, saved, flags, subtract(domain, allowed)))
            covered = []
            for index in range(7):
                selected = intersect(taken, inverse([(index, index)], registers[2]))
                table_domains[index] = normalize(table_domains.get(index, []) + selected)
                covered += selected
                queue.append((pc + 8 + index * 4, saved, flags, selected))
            if normalize(covered) != taken:
                raise FormatError("stock command table index lacks a bounded partition")
            continue
        if (word >> 25) & 7 == 5:
            condition = word >> 28
            if word & (1 << 24) or condition not in (0, 1, 12, 14):
                raise FormatError("unsupported stock command selector branch")
            branch = _a32_branch(payload, pc, False, condition)
            allowed = predicate(flags, condition)
            queue.append((branch["target_blob_file_offset"], saved, flags, intersect(domain, allowed)))
            if condition != 14:
                queue.append((pc + 4, saved, flags, subtract(domain, allowed)))
            continue
        if pc == 0x5f78:
            literal = _a32_literal(payload, pc)
            if (literal["destination_register"], literal["literal_blob_file_offset"]) != (2, 0x6174):
                raise FormatError("unsupported stock command base load")
            registers[2] = (0, literal["literal_value"])
        else:
            if word >> 28 != 14 or (word >> 26) & 3 != 0:
                raise FormatError("unsupported stock command selector instruction")
            operation, source, destination = (word >> 21) & 15, (word >> 16) & 15, (word >> 12) & 15
            value = operand(word, registers)
            if operation == 10 and word & (1 << 20):
                if registers[source] is None:
                    raise FormatError("stock command CMP has unknown input")
                flags = ("cmp", registers[source], value)
            elif operation in (2, 3, 4, 15) and destination != 15:
                if operation == 15:
                    if value[0] != 0 or word & (1 << 20):
                        raise FormatError("unsupported stock command MVN")
                    result = (0, (~value[1]) & mask)
                elif operation == 3:
                    result = affine(value, registers[source], True)
                else:
                    result = affine(registers[source], value, operation == 2)
                registers[destination] = result
                if word & (1 << 20):
                    if operation != 4:
                        raise FormatError("unsupported stock command arithmetic flags")
                    flags = ("zero", result, (0, 0))
            else:
                raise FormatError("unsupported stock command selector opcode")
        queue.append((pc + 4, tuple(registers), flags, domain))

    pieces = sorted(part for domain in terminals.values() for part in domain)
    if (not pieces or pieces[0][0] != 0 or pieces[-1][1] != mask or
            any(left[1] + 1 != right[0] for left, right in zip(pieces, pieces[1:]))):
        raise FormatError("stock command domains are not disjoint and exhaustive")
    routes = []
    for case, domain in sorted(terminals.items()):
        if case == 0x60b8:
            continue
        if len(domain) != 1 or domain[0][0] != domain[0][1]:
            raise FormatError("stock command admitted domain is not a singleton")
        handler = None
        for pc in range(case, case + 32, 4):
            if _bootstrap_word(payload, pc) == 0xe1a00004:
                handler = _a32_branch(payload, pc + 4, True)["target_blob_file_offset"]
                break
        if handler is None:
            raise FormatError("stock command wrapper lacks a direct packet handler")
        routes.append({"command": domain[0][0], "case_blob_file_offset": case,
                       "handler_blob_file_offset": handler})
    routes.sort(key=lambda route: route["command"])
    fallback = terminals.get(0x60b8, [])
    return {"entry_blob_file_offset": 0x5f2c, "domain_bits": 32,
            "accepted_count": len(routes), "fallback_count": sum(b - a + 1 for a, b in fallback),
            "complete_domain": True, "disjoint_domains": True, "routes": routes,
            "fallback_domains": [list(part) for part in fallback],
            "table_blob_file_offset": 0x6008, "table_entry_count": 7,
            "zero_index_reachable": bool(table_domains.get(0)), "cfg_states": states,
            "flag_semantics": "CMP EQ uses Z, CC uses !C, GT uses !Z and N==V; non-S preserves flags; ADDS replaces Z."}


def _stock_host_handler_footprints(payload, entries):
    """Direct packet fields only; opaque callees and external storage stay conditional."""
    ranges = ((0x3ca8, 0x5f2c), (0x6984, 0x69b8), (0x6ab8, 0x6acc))
    mask, results = 0xffffffff, []

    def packet(value):
        return value is not None and value[0] in ("packet", "packet_unknown")

    def stack(value):
        return value is not None and value[0] == "stack"

    def immediate(word):
        value, rotate = word & 0xff, ((word >> 8) & 15) * 2
        return ((value >> rotate) | (value << ((32 - rotate) % 32))) & mask

    def add(left, right, subtract=False):
        if left is None or right is None or right[0] != "constant":
            if stack(left) or stack(right):
                raise FormatError("unsupported stock handler stack arithmetic")
            return ("packet_unknown", 0) if packet(left) or packet(right) else None
        value = left[1] - right[1] if subtract else left[1] + right[1]
        return (left[0], value & mask if left[0] == "constant" else value)

    def operand(word, registers):
        if word & (1 << 25):
            return ("constant", immediate(word))
        value = registers[word & 15]
        if word & (1 << 4):
            amount = registers[(word >> 8) & 15]
            if stack(value) or stack(amount):
                raise FormatError("unsupported stock handler stack register shift")
            return ("packet_unknown", 0) if packet(value) or packet(amount) else None
        shift, kind = (word >> 7) & 31, (word >> 5) & 3
        if not shift and kind == 0:
            return value
        if value is None or value[0] != "constant":
            if stack(value):
                raise FormatError("unsupported stock handler stack shift")
            return ("packet_unknown", 0) if packet(value) else None
        number = value[1]
        if kind == 0:
            number <<= shift
        elif kind == 1:
            number >>= shift or 32
        elif kind == 2:
            number = (number if number < 0x80000000 else number - (1 << 32)) >> (shift or 32)
        elif shift:
            number = (number >> shift) | (number << (32 - shift))
        else:
            return None
        return ("constant", number & mask)

    for entry in sorted(set(entries)):
        initial = [None] * 16
        initial[0], initial[13] = ("packet", 0), ("stack", 0)
        # Dispatcher r4/r6 remain packet/reply aliases across each wrapper.
        initial[4], initial[5], initial[6] = ("packet", 0), ("constant", 8), ("packet", 0x114)
        for register in range(7, 12):
            initial[register] = ("saved_register", register)
        initial[14] = ("return_address", 0)
        facts, queue, stack_facts = {entry: tuple(initial)}, [entry], {entry: {}}
        spills = {}
        reads, writes, callees, external_stores = set(), set(), set(), set()
        header_reads, header_writes = set(), set()
        packet_spills, output_calls = set(), set()
        protected_slots = {}
        steps, returns = 0, set()

        def enqueue(pc, state, spill_state=None):
            if not any(low <= pc < high for low, high in ranges) or pc % 4:
                raise FormatError("stock handler escaped its fixed local CFG")
            state = tuple(state)
            old = facts.get(pc)
            if old is not None and any(a != b and (stack(a) or stack(b))
                                       for a, b in zip(old, state)):
                raise FormatError("unsupported stock handler conditional stack provenance")
            merged = state if old is None else tuple(
                a if a == b else ("packet_unknown", 0) if packet(a) or packet(b) else None
                for a, b in zip(old, state))
            incoming = spills if spill_state is None else spill_state
            old_spills = stack_facts.get(pc)
            merged_spills = dict(incoming) if old_spills is None else {
                slot: old_spills.get(slot) if old_spills.get(slot) == incoming.get(slot) else ("packet_unknown", 0)
                for slot in old_spills.keys() | incoming.keys()}
            if len(merged_spills) > 64:
                raise FormatError("stock handler stack provenance budget exceeded")
            if old != merged or old_spills != merged_spills:
                facts[pc] = merged
                stack_facts[pc] = merged_spills
                queue.append(pc)

        def stack_read(base, offset, width):
            if base is None or base[0] != "stack":
                return None
            position = base[1] + offset
            if width == 4 and position in protected_slots:
                return protected_slots[position][1]
            overlap = [(slot, value) for slot, value in spills.items()
                       if position < slot + 4 and slot < position + width]
            if not overlap:
                return None
            if width == 4 and len(overlap) == 1 and overlap[0][0] == position:
                return overlap[0][1]
            return ("packet_unknown", 0)

        def stack_store(base, offset, width, value, pc, push_floor=None):
            if stack(value):
                raise FormatError("unsupported stock handler stack-pointer spill")
            if base is None or base[0] != "stack":
                if packet(value):
                    raise FormatError("stock handler stores/escapes a packet alias")
                return
            position = base[1] + offset
            current = registers[13]
            floor = push_floor if push_floor is not None else current[1] if current is not None and current[0] == "stack" else None
            if floor is None or not -512 <= floor <= position or position + width > 0:
                raise FormatError("stock handler stack store is outside bounded frame")
            if any(position < slot + 4 and slot < position + width for slot in protected_slots):
                raise FormatError("stock handler overwrites a saved register/return slot")
            for slot in list(spills):
                if position <= slot and slot + 4 <= position + width:
                    del spills[slot]
                elif position < slot + 4 and slot < position + width:
                    spills[slot] = ("packet_unknown", 0)
            if packet(value):
                if width != 4 or position % 4:
                    raise FormatError("stock handler partial packet spill is unsupported")
                spills[position] = value
                packet_spills.add((pc, position, value[1] if value[0] == "packet" else None))

        def access(base, offset, width, load, pc):
            if base is not None and base[0] == "packet_unknown":
                raise FormatError("stock handler packet alias became unbounded")
            if base is None or base[0] != "packet":
                if not load:
                    external_stores.add(pc)
                return
            position = base[1] + offset
            if 0 <= position and position + width <= 0x14:
                (header_reads if load else header_writes).add((position, width))
            elif 0x14 <= position and position + width <= 0x114:
                if not load:
                    raise FormatError("stock handler directly writes its request payload")
                reads.add((position - 0x14, width))
            elif 0x114 <= position and position + width <= 0x214:
                if load:
                    raise FormatError("stock handler directly reads its reply payload")
                if position % 4 or width != 4:
                    raise FormatError("stock handler reply write is not an audited DWORD")
                writes.add((position - 0x114, width))
            else:
                raise FormatError(f"stock handler {entry:#x} packet access at {pc:#x} escaped fields: {position:#x}/{width}")

        while queue:
            pc = queue.pop()
            saved = facts[pc]
            registers = list(saved)
            spills = dict(stack_facts[pc])
            saved_spills = dict(spills)
            steps += 1
            if steps > MAX_STOCK_HOST_COMMAND_CFG_STATES:
                raise FormatError("stock handler CFG state budget exceeded")
            word = _bootstrap_word(payload, pc)
            condition = word >> 28
            if condition == 15:
                raise FormatError("unsupported stock handler unconditional extension")
            if (word >> 25) & 7 == 5:
                if condition == 3:
                    if pc not in (0x5608, 0x5640) or word != 0x3a000000:
                        raise FormatError("unsupported stock handler unsigned branch")
                    target = pc + 8
                else:
                    branch = _a32_branch(payload, pc, bool(word & (1 << 24)), condition)
                    target = branch["target_blob_file_offset"]
                if word & (1 << 24):
                    if target == 0x898:
                        if (_bootstrap_word(payload, 0x898), _bootstrap_word(payload, 0x89c)) != (0xe51f01a4, 0xe12fff1e):
                            raise FormatError("unsupported stock context getter")
                        literal = _a32_literal(payload, 0x898)
                        if (literal["literal_blob_file_offset"], literal["literal_value"],
                                literal["destination_register"]) != (0x6fc, 0xd3a00, 0):
                            raise FormatError("stock context getter literal does not match")
                        callees.add(target)
                        registers[0] = ("constant", literal["literal_value"])
                        registers[14] = ("constant", pc + 4)
                        enqueue(pc + 4, registers)
                        if condition != 14:
                            enqueue(pc + 4, saved)
                        continue
                    if any(packet(value) for value in registers[:4]):
                        raise FormatError(f"stock handler {entry:#x} passes a packet pointer to an opaque callee at {pc:#x}")
                    if target == 0x1bf04:
                        prefix = (0xe92d40f0, 0xe1a06000, 0xe1a04001, 0xe3a07000, 0xe51f1348,
                                  0xe1a00006, 0xebfffec0, 0xe1a05000, 0xe20500ff, 0xe5840000)
                        if any(_bootstrap_word(payload, 0x1bf04 + i * 4) != expected
                               for i, expected in enumerate(prefix)):
                            raise FormatError("unsupported stock START stack-output prefix")
                        literal = _a32_literal(payload, 0x1bf14)
                        if (literal["literal_blob_file_offset"], literal["literal_value"],
                                literal["destination_register"]) != (0x1bbd4, 0x20b008, 1):
                            raise FormatError("stock START output-prefix literal does not match")
                        output = registers[1]
                        if output is None or output[0] != "stack":
                            raise FormatError("stock START output prefix lacks bounded stack destination")
                        # r4 holds the output pointer across the register-read
                        # call (whose args are external object/fixed scalar).
                        # Its return is ANDed255 before STR[r4], replacing the
                        # saved packet DWORD; later body remains opaque.
                        stack_store(output, 0, 4, None, pc)
                        output_calls.add((pc, output[1]))
                    callees.add(target)
                    for register in (0, 1, 2, 3, 12):
                        registers[register] = None
                    registers[14] = ("constant", pc + 4)
                    enqueue(pc + 4, registers)
                    if condition != 14:
                        enqueue(pc + 4, saved, saved_spills)
                else:
                    # The selected dispatcher passes a non-null packet. All
                    # handlers test it with this adjacent CMP/BEQ pair before
                    # forming request/reply aliases; their null diagnostics
                    # are not reachable from an admitted stock route.
                    previous = _bootstrap_word(payload, pc - 4)
                    compared = registers[(previous >> 16) & 15]
                    if (condition in (0, 1) and previous >> 28 == 14 and
                            previous & 0x0ff0ffff == 0x03500000 and
                            compared is not None and compared[0] == "packet" and 0 <= compared[1] < 0x214):
                        enqueue(pc + 4 if condition == 0 else target, registers)
                        continue
                    enqueue(target, registers)
                    if condition != 14:
                        enqueue(pc + 4, saved)
                continue
            if word == 0xe12fff1e or ((word >> 25) & 7 == 4 and word & (1 << 20) and word & (1 << 15)):
                if word != 0xe12fff1e and word & 0x0fff0000 != 0x08bd0000:
                    raise FormatError("unsupported stock handler non-stack return")
                frame = registers[13]
                if frame is None or frame[0] != "stack":
                    raise FormatError("stock handler return lost stack provenance")
                restored = 0 if word == 0xe12fff1e else (word & 0xffff).bit_count() * 4
                if frame[1] + restored != 0:
                    raise FormatError("stock handler return has an unbalanced stack")
                if word == 0xe12fff1e:
                    if registers[14] != ("return_address", 0):
                        raise FormatError("stock handler leaf return lost return-address provenance")
                else:
                    position = 0
                    restored_saved = set()
                    for register in range(16):
                        if word & (1 << register):
                            if register == 0 and packet(stack_read(frame, position, 4)):
                                raise FormatError("stock handler returns a spilled packet alias")
                            slot = frame[1] + position
                            if register == 15:
                                if protected_slots.get(slot) != (14, ("return_address", 0)):
                                    raise FormatError("stock handler POP lost return-address provenance")
                            elif 4 <= register <= 11:
                                if protected_slots.get(slot) != (register, initial[register]):
                                    raise FormatError("stock handler POP lost saved-register provenance")
                                restored_saved.add(register)
                            position += 4
                    if restored_saved != {register for register, _ in protected_slots.values() if 4 <= register <= 11}:
                        raise FormatError("stock handler POP omits a saved register")
                if packet(registers[0]):
                    raise FormatError("stock handler returns a packet alias")
                returns.add(pc)
                if condition != 14:
                    enqueue(pc + 4, saved)
                continue
            if word & 0x0fe00070 == 0x07e00050:  # Selected UBFX, not a memory access.
                destination, source = (word >> 12) & 15, word & 15
                width, shift = ((word >> 16) & 31) + 1, (word >> 7) & 31
                if destination == 15 or source == 15 or width + shift > 32:
                    raise FormatError("unsupported stock handler bit-field extraction")
                value = registers[source]
                if stack(value):
                    raise FormatError("unsupported stock handler stack bit-field extraction")
                registers[destination] = (("constant", (value[1] >> shift) & ((1 << width) - 1))
                                          if value is not None and value[0] == "constant" else
                                          ("packet_unknown", 0) if packet(value) else None)
            elif (word >> 26) & 3 == 1:
                base, destination = (word >> 16) & 15, (word >> 12) & 15
                if word & (1 << 25) and word & (1 << 4):
                    raise FormatError(f"unsupported stock handler media/transfer instruction at {pc:#x}: {word:#x}")
                if destination == 15:
                    raise FormatError("unsupported stock handler load to PC")
                if word & (1 << 25):
                    register_word = word & ~(1 << 25)
                    value = operand(register_word, registers)
                    if value is None or value[0] != "constant":
                        if packet(registers[base]) or stack(registers[base]):
                            raise FormatError("stock handler packet/stack access has an unbounded register offset")
                        offset = None
                    else:
                        offset = value[1]
                else:
                    offset = word & 0xfff
                if offset is not None and not word & (1 << 23):
                    offset = -offset
                load, width = bool(word & (1 << 20)), 1 if word & (1 << 22) else 4
                if destination == base and (word & (1 << 21) or not word & (1 << 24)):
                    raise FormatError("unsupported stock handler overlapping transfer/writeback")
                actual_offset = offset if word & (1 << 24) else 0
                if actual_offset is not None:
                    access(registers[base], actual_offset, width, load, pc)
                elif not load:
                    external_stores.add(pc)
                if load:
                    if base == 15:
                        if width != 4:
                            raise FormatError("unsupported stock handler PC-relative byte load")
                        literal = _a32_literal(payload, pc)
                        registers[destination] = ("constant", literal["literal_value"])
                    else:
                        registers[destination] = (stack_read(registers[base], actual_offset, width)
                                                  if actual_offset is not None else
                                                  ("packet_unknown", 0) if registers[base] is not None and
                                                  registers[base][0] == "stack" and spills else None)
                else:
                    if actual_offset is None:
                        if packet(registers[destination]) or (registers[base] is not None and
                                                             registers[base][0] == "stack" and spills):
                            raise FormatError("stock handler stack/packet spill offset is unbounded")
                    else:
                        stack_store(registers[base], actual_offset, width, registers[destination], pc)
                if word & (1 << 21) or not word & (1 << 24):
                    registers[base] = (add(registers[base], ("constant", offset)) if offset is not None else
                                       ("packet_unknown", 0) if packet(registers[base]) else None)
            elif (word >> 25) & 7 == 4:
                if word & (1 << 20):
                    raise FormatError("unsupported stock handler non-return block load")
                if word & 0x0fff0000 != 0x092d0000:
                    raise FormatError("unsupported stock handler block store")
                if pc != entry:
                    raise FormatError("unsupported stock handler non-prologue PUSH")
                count = (word & 0xffff).bit_count()
                frame = add(registers[13], ("constant", count * 4), True)
                if frame is None or frame[0] != "stack":
                    raise FormatError("stock handler PUSH lacks bounded stack provenance")
                position = 0
                for register in range(16):
                    if word & (1 << register):
                        stack_store(frame, position, 4, registers[register], pc, frame[1])
                        if 4 <= register <= 11 or register == 14:
                            if registers[register] != initial[register]:
                                raise FormatError("stock handler prologue lost incoming saved register")
                            protected_slots[frame[1] + position] = (register, registers[register])
                        position += 4
                registers[13] = frame
            elif (word >> 25) & 7 == 0 and word & 0x90 == 0x90:
                kind = (word >> 5) & 3
                if kind == 0:  # MUL: no packet pointer is multiplied in this contract.
                    destination = (word >> 16) & 15
                    if word & 0x0fe000f0 != 0x00000090 or destination == 15:
                        raise FormatError("unsupported stock handler multiply/atomic instruction")
                    if packet(registers[word & 15]) or packet(registers[(word >> 8) & 15]):
                        raise FormatError("stock handler multiplies a packet alias")
                    if stack(registers[word & 15]) or stack(registers[(word >> 8) & 15]):
                        raise FormatError("stock handler multiplies a stack pointer")
                    registers[destination] = None
                else:
                    base, destination = (word >> 16) & 15, (word >> 12) & 15
                    if word & (1 << 22):
                        offset = ((word >> 4) & 0xf0) | (word & 15)
                    else:
                        value = registers[word & 15]
                        offset = value[1] if value is not None and value[0] == "constant" else None
                    if offset is not None and not word & (1 << 23):
                        offset = -offset
                    double = not word & (1 << 20) and kind in (2, 3)
                    load = bool(word & (1 << 20)) or (double and kind == 2)
                    width = 8 if double else 2 if kind in (1, 3) else 1
                    if destination == 15 or (double and destination >= 14):
                        raise FormatError("unsupported stock handler halfword/double register")
                    actual_offset = offset if word & (1 << 24) else 0
                    if actual_offset is None:
                        if packet(registers[base]) or stack(registers[base]):
                            raise FormatError("stock handler packet/stack halfword offset is unbounded")
                        if not load:
                            external_stores.add(pc)
                    else:
                        access(registers[base], actual_offset, width, load, pc)
                    if load:
                        registers[destination] = (stack_read(registers[base], actual_offset, 4 if double else width)
                                                  if actual_offset is not None else
                                                  ("packet_unknown", 0) if registers[base] is not None and
                                                  registers[base][0] == "stack" and spills else None)
                        if double:
                            registers[destination + 1] = (stack_read(registers[base], actual_offset + 4, 4)
                                                         if actual_offset is not None else registers[destination])
                    else:
                        if actual_offset is None:
                            if packet(registers[destination]) or (double and packet(registers[destination + 1])) or (
                                    registers[base] is not None and registers[base][0] == "stack" and spills):
                                raise FormatError("stock handler stack/packet spill offset is unbounded")
                        elif double:
                            stack_store(registers[base], actual_offset, 4, registers[destination], pc)
                            stack_store(registers[base], actual_offset + 4, 4, registers[destination + 1], pc)
                        else:
                            stack_store(registers[base], actual_offset, width, registers[destination], pc)
                    if word & (1 << 21) or not word & (1 << 24):
                        registers[base] = (add(registers[base], ("constant", offset)) if offset is not None else
                                           ("packet_unknown", 0) if packet(registers[base]) else None)
            elif word & 0x0ff00000 in (0x03000000, 0x03400000):
                destination = (word >> 12) & 15
                value = ((word >> 4) & 0xf000) | (word & 0xfff)
                old = registers[destination]
                if word & 0x0ff00000 == 0x03400000 and stack(old):
                    raise FormatError("unsupported stock handler stack MOVT")
                registers[destination] = (("constant", value) if word & 0x0ff00000 == 0x03000000 else
                                          ("constant", (old[1] & 0xffff) | (value << 16))
                                          if old is not None and old[0] == "constant" else
                                          ("packet_unknown", 0) if packet(old) else None)
            elif (word >> 26) & 3 == 0:
                operation, source, destination = (word >> 21) & 15, (word >> 16) & 15, (word >> 12) & 15
                if operation not in (0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 13, 14, 15):
                    raise FormatError("unsupported stock handler data operation")
                if operation in (8, 9, 10, 11) and (not word & (1 << 20) or destination != 0):
                    raise FormatError("unsupported stock handler status/control instruction")
                if operation not in (8, 9, 10, 11):
                    if destination == 15:
                        raise FormatError("unsupported stock handler computed PC")
                    left = ("constant", pc + 8) if source == 15 else registers[source]
                    right = operand(word, registers)
                    if operation == 13:
                        registers[destination] = right
                    elif operation in (2, 4):
                        registers[destination] = add(left, right, operation == 2)
                    elif operation == 3:
                        registers[destination] = add(right, left, True)
                    elif operation == 15:
                        if stack(right):
                            raise FormatError("unsupported stock handler stack MVN")
                        registers[destination] = (("constant", ~right[1] & mask)
                                                  if right is not None and right[0] == "constant" else
                                                  ("packet_unknown", 0) if packet(right) else None)
                    elif left is not None and right is not None and left[0] == right[0] == "constant":
                        a, b = left[1], right[1]
                        number = {0: a & b, 1: a ^ b, 12: a | b, 14: a & ~b}[operation]
                        registers[destination] = ("constant", number & mask)
                    else:
                        if stack(left) or stack(right):
                            raise FormatError("unsupported stock handler stack bit operation")
                        registers[destination] = ("packet_unknown", 0) if packet(left) or packet(right) else None
            else:
                raise FormatError("unsupported stock handler instruction")
            enqueue(pc + 4, registers)
            if condition != 14:
                enqueue(pc + 4, saved, saved_spills)
        if not returns:
            raise FormatError("stock handler has no bounded local return edge")
        results.append({"entry_blob_file_offset": entry,
                        "request_reads": [{"byte_offset": offset, "width": width} for offset, width in sorted(reads)],
                        "reply_writes": [{"word_index": offset // 4, "byte_offset": offset, "width": width}
                                         for offset, width in sorted(writes)],
                        "callee_targets": sorted(callees), "direct_request_reply_only": True,
                        "packet_header_reads": [{"byte_offset": offset, "width": width}
                                                for offset, width in sorted(header_reads)],
                        "packet_header_writes": [{"byte_offset": offset, "width": width}
                                                 for offset, width in sorted(header_writes)],
                        "local_instruction_count": len(facts), "cfg_states": steps,
                        "return_blob_file_offsets": sorted(returns),
                        "stack_packet_spills": [{"instruction_blob_file_offset": pc, "frame_byte_offset": slot,
                                                 "packet_byte_offset": value} for pc, slot, value in
                                                sorted(packet_spills, key=lambda item: (item[0], item[1], -1 if item[2] is None else item[2]))],
                        "validated_stack_output_prefix_calls": [
                            {"call_blob_file_offset": pc, "frame_byte_offset": slot,
                             "overwritten_bytes": 4, "value_mask": 255} for pc, slot in sorted(output_calls)],
                        "conditional_external_store_count": len(external_stores)})
    return results


def _stock_host_reply_clear(payload):
    """The actual zero/256-byte call, for every destination alignment."""
    expected = {0x5f44: 0xe2845014, 0x5f48: 0xe2846f45, 0x5f4c: 0xe3002100,
                0x5f50: 0xe3a01000, 0x5f54: 0xe1a00006, 0x5f58: 0xeb0069e1,
                0x206e4: 0xe92d4070, 0x206e8: 0xe1a06000, 0x206ec: 0xe1a04001,
                0x206f0: 0xe1a05002, 0x206f4: 0xe1a02004, 0x206f8: 0xe1a01005,
                0x206fc: 0xe1a00006, 0x20700: 0xfa002fe0, 0x20704: 0xe8bd8070}
    for pc, word in expected.items():
        if _bootstrap_word(payload, pc) != word:
            raise FormatError("unsupported stock reply-clear ARM instruction")
    if (_bootstrap_word(payload, 0x5f5c), _bootstrap_word(payload, 0x5f60)) != (0xe5950000, 0xe5860000):
        raise FormatError("unsupported stock reply command echo")
    if bounded(payload, 0x2c688, 16, "stock clear expansion") != bytes.fromhex(
            "02f0ff0343ea032242ea024200f052b8"):
        raise FormatError("unsupported stock reply-clear byte expansion")
    # Explicit audited Thumb instructions, not a general Thumb decoder.
    ops = (
        (0x2c73c, "0429", "cmp4"), (0x2c73e, "c0f01280", "bcc_small"),
        (0x2c742, "10f0030c", "alignment"), (0x2c746, "00f01b80", "beq_bulk"),
        (0x2c74a, "ccf1040c", "prefix"), (0x2c74e, "bcf1020f", "cmp2"),
        (0x2c752, "18bf", "it_ne"), (0x2c754, "00f8012b", "byte"),
        (0x2c758, "a8bf", "it_ge"), (0x2c75a, "20f8022b", "half"),
        (0x2c75e, "a1eb0c01", "subtract_prefix"), (0x2c762, "00f00db8", "bulk"),
        (0x2c766, "5fea c17c".replace(" ", ""), "shift31"),
        (0x2c76a, "24bf", "itt_cs"), (0x2c76c, "00f8012b", "byte"),
        (0x2c770, "00f8012b", "byte"), (0x2c774, "48bf", "it_mi"),
        (0x2c776, "00f8012b", "byte"), (0x2c77a, "7047", "return"),
        (0x2c77c, "4ff00002", "zero_entry"),
        (0x2c780, "00b5", "push_lr"), (0x2c782, "1346", "copy_r3"),
        (0x2c784, "9446", "copy_ip"), (0x2c786, "9646", "copy_lr"),
        (0x2c788, "2039", "subtract32"), (0x2c78a, "22bf", "ittt_cs"),
        (0x2c78c, "a0e80c50", "stm16"), (0x2c790, "a0e80c50", "stm16"),
        (0x2c794, "b1f12001", "subtract32"), (0x2c798, "bff4f7af", "bcs_loop"),
        (0x2c79c, "0907", "shift28"), (0x2c79e, "28bf", "it_cs"),
        (0x2c7a0, "a0e80c50", "stm16"), (0x2c7a4, "48bf", "it_mi"),
        (0x2c7a6, "0cc0", "stm8"), (0x2c7a8, "5df804eb", "pop_lr"),
        (0x2c7ac, "8900", "shift2"), (0x2c7ae, "28bf", "it_cs"),
        (0x2c7b0, "40f8042b", "word"), (0x2c7b4, "08bf", "it_eq"),
        (0x2c7b6, "7047", "return"), (0x2c7b8, "48bf", "it_mi"),
        (0x2c7ba, "20f8022b", "half"), (0x2c7be, "11f0804f", "tst_bit30"),
        (0x2c7c2, "18bf", "it_ne"), (0x2c7c4, "00f8012b", "byte"),
        (0x2c7c8, "7047", "return"))
    instructions = {}
    for pc, raw, operation in ops:
        encoded = bytes.fromhex(raw)
        if bounded(payload, pc, len(encoded), "stock clear Thumb instruction") != encoded:
            raise FormatError("unsupported stock reply-clear Thumb instruction")
        instructions[pc] = (len(encoded), operation)
    mask, alignments = 0xffffffff, []
    for alignment in range(4):
        pc, destination, remaining, ip = 0x2c73c, alignment, 256, 0
        n, z, c, v = False, False, False, False
        guards, writes, saved_lr, steps = [], [], False, 0
        r2, r3, lr_value = 0, None, None

        def compare(left, right):
            result = (left - right) & mask
            return (bool(result & 0x80000000), result == 0, left >= right,
                    bool(((left ^ right) & (left ^ result)) & 0x80000000))

        def allowed(condition):
            return {"ne": not z, "ge": n == v, "cs": c, "mi": n, "eq": z}[condition]

        while True:
            steps += 1
            if steps > MAX_STOCK_HOST_COMMAND_CFG_STATES or pc not in instructions:
                raise FormatError("stock reply-clear instruction/state budget exceeded")
            size, operation = instructions[pc]
            next_pc = pc + size
            if guards and not allowed(guards.pop(0)):
                pc = next_pc
                continue
            if operation.startswith("it"):
                count = 3 if operation.startswith("ittt_") else 2 if operation.startswith("itt_") else 1
                if guards:
                    raise FormatError("unsupported nested stock clear IT block")
                guards = [operation.rsplit("_", 1)[1]] * count
            elif operation == "cmp4":
                n, z, c, v = compare(remaining, 4)
            elif operation == "bcc_small":
                if not c:
                    next_pc = 0x2c766
            elif operation == "alignment":
                ip = destination & 3
                n, z = False, ip == 0
            elif operation == "beq_bulk":
                if z:
                    next_pc = 0x2c780
            elif operation == "prefix":
                ip = (4 - ip) & mask
            elif operation == "cmp2":
                n, z, c, v = compare(ip, 2)
            elif operation == "subtract_prefix":
                remaining = (remaining - ip) & mask
            elif operation == "bulk":
                next_pc = 0x2c780
            elif operation in ("byte", "half", "word", "stm8", "stm16"):
                width = {"byte": 1, "half": 2, "word": 4, "stm8": 8, "stm16": 16}[operation]
                values = [r2, r3, ip, lr_value] if operation == "stm16" else [r2, r3] if operation == "stm8" else [r2]
                if any(value != 0 for value in values):
                    raise FormatError("stock reply clear stores a nonzero or unknown value")
                offset = destination - alignment
                if not 0 <= offset <= 256 - width:
                    raise FormatError("stock reply clear writes outside its exact span")
                writes.append([offset, width])
                destination += width
            elif operation == "subtract32":
                n, z, c, v = compare(remaining, 32)
                remaining = (remaining - 32) & mask
            elif operation == "bcs_loop":
                if c:
                    next_pc = 0x2c78a
            elif operation in ("shift31", "shift28", "shift2"):
                shift = {"shift31": 31, "shift28": 28, "shift2": 2}[operation]
                shifted = (remaining << shift) & mask
                c, n, z = bool((remaining >> (32 - shift)) & 1), bool(shifted & 0x80000000), shifted == 0
                if operation == "shift31":
                    ip = shifted
                else:
                    remaining = shifted
            elif operation == "push_lr":
                if saved_lr:
                    raise FormatError("stock reply-clear stack is unbalanced")
                saved_lr = True
            elif operation == "pop_lr":
                if not saved_lr:
                    raise FormatError("stock reply-clear stack is unbalanced")
                saved_lr = False
                lr_value = None  # The real return address is restored, not stored.
            elif operation == "tst_bit30":
                z, n = not bool(remaining & 0x40000000), False
                c = False  # Modified immediate 0x40000000 has bit31 clear.
            elif operation == "copy_r3":
                r3 = r2
            elif operation == "copy_ip":
                ip = r2
            elif operation == "copy_lr":
                lr_value = r2
            elif operation == "zero_entry":
                raise FormatError("stock clear reached a different entry")
            elif operation == "return":
                if saved_lr or guards:
                    raise FormatError("stock reply-clear return has outstanding state")
                break
            else:
                raise FormatError("unsupported stock reply-clear operation")
            pc = next_pc
        cursor = 0
        for offset, width in writes:
            if offset != cursor:
                raise FormatError("stock reply-clear footprint overlaps or has holes")
            cursor += width
        if cursor != 256:
            raise FormatError("stock reply clear is not exactly 256 bytes")
        alignments.append({"destination_alignment": alignment, "write_ranges": writes,
                           "written_bytes": cursor, "complete": True, "overlap": False,
                           "instruction_steps": steps})
    return {"request_record_offset": 0x14, "reply_record_offset": 0x114,
            "byte_count": 256, "byte_value": 0, "alignments": alignments,
            "command_echo": {"request_word_index": 0, "reply_word_index": 0,
                             "load_blob_file_offset": 0x5f5c, "store_blob_file_offset": 0x5f60}}


def _stock_host_command_closure(payload):
    """Private conditional stock host contract; never a device/raw-frame API."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("stock host command payload identity/size does not match")
    regions = _STOCK_HOST_COMMAND_REGIONS
    total = sum(size for _, _, size, _ in regions)
    if (len(regions) > MAX_STOCK_HOST_COMMAND_REGIONS or total > MAX_STOCK_HOST_COMMAND_BYTES or
            MAX_STOCK_HOST_COMMAND_CFG_STATES < 1 or MAX_STOCK_HOST_COMMAND_PARTITIONS < 1):
        raise FormatError("stock host command validation budget exceeded")
    validated = []
    # Every selected byte, including skipped arms and helper literals, must
    # pass before ANY instruction/field/domain interpretation begins.
    for role, offset, size, expected in regions:
        actual = bounded(payload, offset, size, "stock host command region")
        if hashlib.sha256(actual).hexdigest() != expected:
            raise FormatError(f"stock host command region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": expected})
    dispatch = _stock_host_dispatch_domains(payload)
    if (dispatch["accepted_count"] != 29 or dispatch["fallback_count"] != (1 << 32) - 29 or
            dispatch["zero_index_reachable"]):
        raise FormatError("stock host command selector does not match the complete baseline domain")
    entries = [route["handler_blob_file_offset"] for route in dispatch["routes"]]
    handlers = _stock_host_handler_footprints(payload, entries)
    expected_requests = {
        0x3ca8: ((4, 4), (8, 4), (12, 4)),
        0x3fb0: ((4, 4), (8, 4), (12, 1)),
        0x41bc: ((4, 4), (8, 4), (12, 4)),
        0x4288: ((4, 4), (8, 4)), 0x4630: ((4, 4), (8, 4)),
        0x4a60: ((4, 4), (8, 4), (12, 4), (16, 1)),
        0x4bd4: ((4, 4), (8, 4)), 0x4c7c: ((4, 4), (8, 4), (12, 4)),
        0x4f88: ((4, 4), (8, 4)),
        0x51c8: ((4, 4), (16, 1), (32, 4), (36, 1), (56, 4))}
    controls = {0x3ca8, 0x3fb0, 0x41bc, 0x4288, 0x4630, 0x4a60, 0x4c7c, 0x4f88, 0x5ccc}
    header_flag_handlers = {0x3fb0, 0x41bc, 0x4288, 0x4a60, 0x4bd4, 0x4c7c}
    classifications = {}
    for handler in handlers:
        entry = handler["entry_blob_file_offset"]
        request = tuple((field["byte_offset"], field["width"]) for field in handler["request_reads"])
        reply = [field["word_index"] for field in handler["reply_writes"]]
        expected_reply = [1, 2, 3, 11] if entry == 0x51c8 else [1, 2, 3, 4, 5] if entry == 0x5ba4 else [1, 2]
        expected_header = [{"byte_offset": 16, "width": 1}] if entry in header_flag_handlers else []
        if (request != expected_requests.get(entry, ((4, 4),)) or reply != expected_reply or
                handler["packet_header_reads"] or handler["packet_header_writes"] != expected_header):
            raise FormatError("stock host command direct field footprint does not match")
        classification = ("compressed_input_open" if entry == 0x51c8 else "version_metadata" if entry == 0x5ba4 else
                          "started_ack_only" if entry == 0x4bd4 else "control" if entry in controls else "ack_only")
        handler["classification"] = classification
        classifications[entry] = classification
    if len(handlers) != 25:
        raise FormatError("stock host command unique handler count does not match")
    for route in dispatch["routes"]:
        route["classification"] = classifications[route["handler_blob_file_offset"]]
    clear = _stock_host_reply_clear(payload)
    return {
        "validation": {"region_count": len(validated), "bytes": total, "regions": validated},
        "dispatch": dispatch, "handlers": handlers, "reply_initialization": clear,
        "source_context": {
            "compressed_tx_metadata_reply_word": 11,
            "open_reply_stores_blob_file_offsets": [0x5914, 0x591c],
            "tx_metadata_address_expression": "word at ARM MMIO 0x100f6004 + 0x301; no plane extent returned",
            "driver_open_postprocessing": "driver/linux/crystalhd_fleafuncs.c:1867",
            "tx_layout_header": "include/flea/DriverFwShare.h",
            "tx_window_kind": "Bounded compressed-input DRAM windows, not a raw-source-plane lease.",
            "getter": {"entry_blob_file_offset": 0x898, "literal_blob_file_offset": 0x6fc,
                       "fixed_context_value": 0xd3a00, "incoming_arguments_read": False},
            "caller": {"entry_blob_file_offset": 0x9048, "dispatch_call_blob_file_offset": 0x9204,
                       "direct_payload_read": "command word only; queue/header flags are separate",
                       "unknown_preclassification_still_queued": True},
            "queue_publication": {"entry_blob_file_offset": 0x8afc,
                                  "node_write_byte_offsets": [0, 4, 8, 12],
                                  "reply_payload_written": False, "packet_header_overlay_bytes": 16,
                                  "request_reply_payloads_disjoint": True, "whole_packet_disjoint": False},
            "start_stack_output_prefix": {"entry_blob_file_offset": 0x1bf04,
                                          "last_validated_blob_file_offset": 0x1bf28,
                                          "nested_callee_blob_file_offset": 0x1ba24,
                                          "literal_blob_file_offset": 0x1bbd4,
                                          "literal_value": 0x20b008, "stored_value_mask": 255,
                                          "post_prefix_body_validated": False}},
        "assumptions": [
            "Non-null packet has readable request256 bytes and writable reply256 bytes at +0x14/+0x114; its full532-byte span does not wrap u32.",
            "Valid initialized channel IDs/objects and ordinary stock control states; malformed scalar channels are not a raw-buffer interface.",
            "Opaque callees return, preserve callee-saved registers/SP under the calling convention, and do not mutate packet fields through undisclosed aliases.",
            "Ordinary valid packet storage follows the stock address map: all modeled external-store destinations, including firmware globals/context, stack, MMIO and compressed TX metadata, do not alias request/reply payload spans.",
            "Intrusive queue nodes intentionally overlay the first16 packet-header bytes; well-formed queue link/header writes remain separate from the request/reply payload spans, not from the whole packet.",
            "Opaque callees do not inspect saved/local non-argument stack aliases or introduce packet aliases via undisclosed memory reads. START's pinned output prefix overwrites its saved packet DWORD; register-read and post-prefix bodies remain opaque.",
            "Opaque callees preserve protected caller stack slots, do not return packet aliases, and keep explicitly passed stack output within allocated scratch storage; wrapper logging arguments/callees remain unmodeled under the same no-undisclosed-alias condition.",
            "The direct-field CFG conservatively explores unknown conditions; only adjacent null-packet tests are pruned under the non-null assumption.",
            "The fixed context getter ignores incoming arguments; all other callee bodies remain unvalidated."],
        "validation_scope": {"full_stock_selector": True, "direct_handler_footprints": True,
                             "exact_reply_clear": True, "callee_bodies": False, "runtime_observed": False,
                             "source_plane_ownership": False, "whole_firmware_absence": False},
        "conclusion": {"conditional": True, "explicit_raw_source_plane_lease": False,
                       "compressed_tx_metadata_is_raw_plane_lease": False,
                       "scope": "No explicit bounded caller raw-source-plane lease in this stock dispatch contract; source-plane lifecycle and runtime ownership are not proven. Not silicon incapability, arbitrary host-buffer absence, whole-firmware ownership, or standalone execution."}}


def _debug_mechanism_map(payload, images):
    """Separate pinned stock debug mechanisms without claiming live access."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("debug-mechanism payload size does not match the bundled baseline")
    identities = [(0x2ea60, 0x79dd8, 32, "little", 45, 2, 0, 0x3a678),
                  (0x79dd8, 0xcfbb0, 32, "little", 45, 2, 0, 0x49f68)]
    fields = ("blob_file_offset", "blob_file_end", "class", "endianness",
              "machine", "elf_type", "flags", "entry_virtual_address")
    if [tuple(image.get(field) for field in fields) for image in images] != identities:
        raise FormatError("debug-mechanism ELF identities do not match the bundled baseline")

    regions = _DEBUG_MECHANISM_REGIONS
    total = sum(size for _, _, size, _ in regions)
    if len(regions) > MAX_DEBUG_MECHANISM_REGIONS or total > MAX_DEBUG_MECHANISM_BYTES:
        raise FormatError("debug-mechanism validation budget exceeded")
    validated = []
    # Validate every selected byte before interpreting branches, literals,
    # messages, register writes or retained symbol identities.
    for role, offset, size, expected in regions:
        actual = bounded(payload, offset, size, "debug-mechanism region")
        if hashlib.sha256(actual).hexdigest() != expected:
            raise FormatError(f"debug-mechanism region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": size,
                          "sha256": expected})

    # This validates the complete stock-command pin set before the DEBUG_SETUP
    # branch below is decoded. Its own helper has the same validate-then-model
    # ordering for the selector and handler footprint.
    stock = _stock_host_command_closure(payload)

    expected_symbols = (
        {
            "Arc_UartInit": (0x23e84, 0x46a18, 156, 2, ".text"),
            "Arc_UartPoll": (0x23f20, 0x46ab4, 28, 2, ".text"),
            "ArcGetc": (0x23f9c, 0x46b30, 40, 2, ".text"),
            "ArcPutc": (0x8000, 0x32ea4, 144, 2, ".core_critical_code_picture"),
            "ArcCommandBuffer": (0x23e48, 0x469dc, 12, 2, ".text"),
            "ReadLine": (0x27ae4, 0x4a678, 292, 2, ".text"),
            "MatchKeyword": (0x27c08, 0x4a79c, 108, 2, ".text"),
            "CmdPeek": (0x28e84, 0x4ba18, 232, 2, ".text"),
            "CmdChannelDramLogControl": (0x255a4, 0x48138, 344, 2, ".text"),
            "CmdChannelDramLogCmd": (0x256fc, 0x48290, 168, 2, ".text"),
            "WritetoDramLogBuffer": (0x7f8c, 0x32e30, 116, 2,
                                     ".core_critical_code_picture"),
        },
        {
            "Arc_UartInit": (0x41104, 0xb8905, 156, 2, ".text"),
            "Arc_UartPoll": (0x411a0, 0xb89a1, 28, 2, ".text"),
            "ArcGetc": (0x41234, 0xb8a35, 40, 2, ".text"),
            "ArcPutc": (0x411bc, 0xb89bd, 32, 2, ".text"),
            "ReadLine": (0x4254c, 0xb9d4d, 292, 2, ".text"),
            "MatchKeyword": (0x42670, 0xb9e71, 108, 2, ".text"),
            "CmdPeek": (0x42c7c, 0xba47d, 232, 2, ".text"),
        },
    )
    inventories = []
    names = set(_DEBUG_MECHANISM_SYMBOLS)
    for image, expected in zip(images, expected_symbols):
        selected = [symbol for symbol in image["symbols"] if symbol["name"] in names]
        if len({symbol["name"] for symbol in selected}) != len(selected):
            raise FormatError("debug-mechanism symbol name is duplicated")
        actual = {symbol["name"]: (symbol["elf_virtual_address"], symbol["blob_file_offset"],
                                    symbol["size"], symbol["type"], symbol["section"])
                  for symbol in selected}
        if actual != expected:
            raise FormatError("debug-mechanism symbols do not match the bundled baseline")
        inventories.append({name: {"elf_virtual_address": values[0],
                                   "blob_file_offset": values[1], "size": values[2],
                                   "type": values[3], "section": values[4]}
                            for name, values in expected.items()})

    branches = {
        "bootstrap_to_router": _a32_branch(payload, 0x2cbf0, link=True),
        "router_to_uart_setup": _a32_branch(payload, 0xac54, link=True),
        "debug_setup_to_log": _a32_branch(payload, 0x5a20, link=True),
        "log_to_arm_uart_formatter": _a32_branch(payload, 0x203d0, link=True),
        "formatter_disabled_skip": _a32_branch(payload, 0xafb8, condition=0),
        "formatter_to_uart_puts": _a32_branch(payload, 0xafd4, link=True),
    }
    expected_targets = {"bootstrap_to_router": 0xac1c, "router_to_uart_setup": 0xadf0,
                        "debug_setup_to_log": 0x203c4,
                        "log_to_arm_uart_formatter": 0xafa0,
                        "formatter_disabled_skip": 0xafdc,
                        "formatter_to_uart_puts": 0xaf5c}
    if any(branches[name]["target_blob_file_offset"] != target
           for name, target in expected_targets.items()):
        raise FormatError("debug-mechanism branch target does not match the baseline")
    literals = {offset: _bootstrap_word(payload, offset)
                for offset in (0xac6c, 0xac70, 0xac74, 0xac78,
                               0xb034, 0xb038, 0xb03c, 0xb040)}
    if literals != {0xac6c: 0x00111111, 0xac70: 0x10404000,
                    0xac74: 115200, 0xac78: 108000000,
                    0xb034: 0x100f3000, 0xb038: 0xd2250,
                    0xb03c: 0x10404000, 0xb040: 0xd2210}:
        raise FormatError("debug-mechanism literal does not match the baseline")

    routes = [route for route in stock["dispatch"]["routes"]
              if route["command"] == 0x73763006]
    handlers = [handler for handler in stock["handlers"]
                if handler["entry_blob_file_offset"] == 0x5a08]
    if len(routes) != 1 or len(handlers) != 1:
        raise FormatError("DEBUG_SETUP route is missing or ambiguous")
    route, handler = routes[0], handlers[0]
    if ((route["case_blob_file_offset"], route["handler_blob_file_offset"],
         route["classification"]) != (0x62ac, 0x5a08, "ack_only") or
            handler["callee_targets"] != [0x203c4] or
            handler["request_reads"] != [{"byte_offset": 4, "width": 4}] or
            handler["reply_writes"] != [{"byte_offset": 4, "width": 4, "word_index": 1},
                                         {"byte_offset": 8, "width": 4, "word_index": 2}] or
            not handler["direct_request_reply_only"]):
        raise FormatError("DEBUG_SETUP direct footprint does not match the baseline")
    message = bounded(payload, 0x5b34, 46, "DEBUG_SETUP message")
    if not message.endswith(b"\0"):
        raise FormatError("DEBUG_SETUP message is not terminated")

    def records(inventory, selected_names):
        return [{"name": name, **inventory[name]} for name in selected_names]

    uart_names = ("Arc_UartInit", "Arc_UartPoll", "ArcGetc", "ArcPutc")
    parser_names = ("ReadLine", "MatchKeyword", "CmdPeek")
    dram_names = ("CmdChannelDramLogControl", "CmdChannelDramLogCmd",
                  "WritetoDramLogBuffer")
    return {
        "schema_version": 1,
        "kind": "stock-debug-mechanism-separation",
        "validation": {"region_count": len(validated), "bytes": total,
                       "regions": validated, "elf_images": len(images)},
        "public_c011_debug_setup": {
            "command": 0x73763006, "case_blob_file_offset": 0x62ac,
            "handler_blob_file_offset": 0x5a08, "classification": "ack_only",
            "request_reads": handler["request_reads"],
            "reply_writes": handler["reply_writes"],
            "message_blob_file_offset": 0x5b34,
            "message": message[:-1].decode("ascii"),
            "logging_bridge_blob_file_offset": 0x203c4,
            "direct_uart_configuration": False,
            "logging_calls_arm_uart_formatter": True,
            "uart_bytes_emitted_unconditionally": False,
        },
        "arm_uart": {
            "architecture": "A32", "bootstrap_call": branches["bootstrap_to_router"],
            "router_setup_entry_blob_file_offset": 0xac1c,
            "uart_setup_call": branches["router_to_uart_setup"],
            "uart_setup_entry_blob_file_offset": 0xadf0,
            "firmware_mmio_base": 0x100f3000,
            "register_offsets": {"data": 0, "control": 4, "status": 8},
            "bootstrap_input_clock_hz": 108000000,
            "bootstrap_baud_rate": 115200,
            "pin_mux_base": 0x10404000, "pin_mux_offset": 0x100,
            "pin_mux_value": 0x00111111,
            "router_offset": 0x21c, "router_value": 0x321,
            "router_ports": [{"port": 0, "source": "ARM", "selector": 1},
                             {"port": 1, "source": "AVD0_OL", "selector": 2},
                             {"port": 2, "source": "AVD0_IL", "selector": 3}],
            "debug_setup_log_call": branches["debug_setup_to_log"],
            "log_formatter_call": branches["log_to_arm_uart_formatter"],
            "formatter_output_call": branches["formatter_to_uart_puts"],
            "formatter_disabled_skip": branches["formatter_disabled_skip"],
            "formatter_enable_byte_address": 0xd2210,
            "formatter_output_is_enable_gated": True,
            "rdb_headers": ["bchp_arm_uart.h", "bchp_sun_top_ctrl.h"],
        },
        "arc_uart": {
            "architecture": "ARC", "outer_image_index": 0, "inner_image_index": 1,
            "outer_symbols": records(inventories[0], uart_names + parser_names +
                                     ("ArcCommandBuffer",)),
            "inner_symbols": records(inventories[1], uart_names + parser_names),
            "routed_ports": {"outer": 1, "inner": 2},
            "symbol_identity_proves_runtime_accessibility": False,
            "symbol_identity_proves_accepted_command_syntax": False,
        },
        "dram_log_debug_commands": {
            "architecture": "ARC", "owner_image_index": 0,
            "symbols": records(inventories[0], dram_names),
            "same_entry_as_public_c011_handler": False,
            "reachability_from_public_debug_setup_established": False,
            "activation_or_return_buffer_path_established": False,
        },
        "separation": {
            "public_command_is_uart_configuration": False,
            "public_command_logging_reaches_arm_uart_formatter": True,
            "public_command_uart_output_is_conditional": True,
            "arm_and_arc_uart_implementations_are_distinct": True,
            "outer_and_inner_arc_uart_images_are_distinct": True,
            "dram_log_commands_are_outer_arc_symbols": True,
        },
        "scope": {
            "exact_bundled_regions": True, "complete_public_selector_domain": True,
            "direct_debug_setup_footprint": True, "retained_symbol_identity": True,
            "complete_arm_call_graph": False, "complete_arc_call_graph": False,
            "runtime_uart_accessibility": False, "live_uart_output": False,
            "accepted_arc_command_syntax": False, "dram_log_runtime_activation": False,
        },
        "assumptions": [
            "The public command classification inherits the stock closure's valid-packet, calling-convention and no-undisclosed-alias assumptions.",
            "Retained ARC function symbols identify stored image objects only; they do not prove execution, UART RX availability or command reachability.",
            "Firmware register addresses and RDB selector names are static provenance, not a live routing observation.",
        ],
    }


def _rx_descriptor_admission_map(payload):
    """Pinned Y-RX descriptor publication, not DMA completion."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("RX descriptor-admission payload size does not match the bundled baseline")
    regions = _RX_DESCRIPTOR_ADMISSION_REGIONS
    total = sum(size for _, _, size, _ in regions)
    if (len(regions) > MAX_RX_DESCRIPTOR_ADMISSION_REGIONS or
            total > MAX_RX_DESCRIPTOR_ADMISSION_BYTES):
        raise FormatError("RX descriptor-admission validation budget exceeded")
    validated = []
    # Pin the complete selected body, its caller and its MMIO literal before
    # decoding any instruction or assigning register semantics.
    for role, offset, size, expected in regions:
        data = bounded(payload, offset, size, "RX descriptor-admission region")
        if hashlib.sha256(data).hexdigest() != expected:
            raise FormatError(f"RX descriptor-admission region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset,
                          "size": size, "sha256": expected})

    words = {
        offset: _bootstrap_word(payload, offset) for offset in (
            0x77e0, 0x77f0, 0x77f8, 0x7800, 0x780c, 0x7810, 0x7814,
            0x7818, 0x7828, 0x782c, 0x7830, 0x784c, 0x7858, 0x785c,
            0x7860, 0x7864, 0x7874, 0x7878, 0x787c, 0x8818, 0x8820)
    }
    expected_words = {
        0x77e0: 0xe92d4070, 0x77f0: 0xe5940000, 0x77f8: 0xe3500000,
        0x7800: 0xe9940006, 0x780c: 0xe5940008, 0x7810: 0xe585004c,
        0x7814: 0xe5940004, 0x7818: 0xe3500000, 0x7828: 0xe5940004,
        0x782c: 0xe3800001, 0x7830: 0xe5850048, 0x784c: 0xe9940006,
        0x7858: 0xe5940008, 0x785c: 0xe5850044, 0x7860: 0xe5940004,
        0x7864: 0xe3500000, 0x7874: 0xe5940004, 0x7878: 0xe3800001,
        0x787c: 0xe5850040, 0x8818: 0xe2840f62, 0x8820: 0xe5c471a8,
    }
    if words != expected_words:
        raise FormatError("RX descriptor-admission instruction does not match the baseline")

    literal = _a32_literal(payload, 0x77f4)
    if (literal["literal_blob_file_offset"], literal["literal_value"],
            literal["destination_register"]) != (0x79b0, 0x10502000, 5):
        raise FormatError("RX descriptor-admission MMIO literal does not match the baseline")
    branches = {}
    for name, site, target, link, condition in (
            ("caller", 0x881c, 0x77e0, True, 14),
            ("selector_zero", 0x77fc, 0x784c, False, 0),
            ("list1_low_zero", 0x781c, 0x7838, False, 0),
            ("list0_low_zero", 0x7868, 0x7884, False, 0),
            ("list0_join", 0x7880, 0x7834, False, 14),
            ("list1_zero_tail", 0x7848, 0x203c4, False, 14),
            ("list0_zero_tail", 0x7894, 0x203c4, False, 14)):
        branch = _a32_branch(payload, site, link=link, condition=condition)
        if branch["target_blob_file_offset"] != target:
            raise FormatError("RX descriptor-admission branch target does not match the baseline")
        branches[name] = branch
    calls = []
    for role, site, target in (
            ("uart_character_output", 0x77ec, 0xaf18),
            ("log", 0x7808, 0x203c4),
            ("uart_character_output", 0x7824, 0xaf18),
            ("uart_character_output", 0x783c, 0xaf18),
            ("log", 0x7854, 0x203c4),
            ("uart_character_output", 0x7870, 0xaf18),
            ("uart_character_output", 0x7888, 0xaf18)):
        branch = _a32_branch(payload, site, link=True)
        if branch["target_blob_file_offset"] != target:
            raise FormatError("RX descriptor-admission call target does not match the baseline")
        calls.append({"role": role, **branch})
    tail_calls = [
        {"role": "log", "tail_call": True, **branches[name]}
        for name in ("list1_zero_tail", "list0_zero_tail")
    ]

    return {
        "schema_version": 1, "kind": "stock-y-rx-descriptor-admission",
        "isa": "A32", "endianness": "little", "device_observed": False,
        "validation": {"region_count": len(validated), "bytes": total,
                       "regions": validated},
        "caller": {
            "record_expression": "channel + 0x188",
            "record_address_instruction_blob_file_offset": 0x8818,
            "call": branches["caller"],
            "pending_byte_offset": 0x1a8,
            "pending_store_instruction_blob_file_offset": 0x8820,
            "pending_store_source_register": 7,
            "pending_store_value_established_by_caller_triplet": False,
            "runtime_channel_identity_established": False,
        },
        "input_record": {
            "source": "include/flea/DriverFwShare.h:22",
            "byte_count_available": 32, "byte_count_read_by_selected_body": 12,
            "selector_word_offset": 0, "y_low_word_offset": 4,
            "y_high_word_offset": 8, "remaining_words_read": False,
            "record_stability_during_repeated_low_reads_assumed": True,
        },
        "selector": {
            "zero_selects": 0, "every_nonzero_selects": 1,
            "accepted_domain_validated": False,
            "zero_branch": branches["selector_zero"],
        },
        "publication": {
            "firmware_mmio_base": literal["literal_value"],
            "rdb_base": 0x00502000,
            "rdb_source": "include/flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_misc1.h",
            "list0": {"high_address": 0x10502044, "high_store": 0x785c,
                      "low_address": 0x10502040, "low_valid_store": 0x787c},
            "list1": {"high_address": 0x1050204c, "high_store": 0x7810,
                      "low_address": 0x10502048, "low_valid_store": 0x7830},
            "valid_mask": 1,
            "low_transform": "u32(low | 1); no alignment normalization",
            "direct_program_order": ["selected high DWORD", "selected low DWORD OR VALID"],
            "low_zero_direct_effect": "Selected high DWORD is written; selected low/VALID register is unchanged.",
            "unselected_list_unchanged_by_selected_body": True,
        },
        "opaque_calls": calls,
        "opaque_tail_calls": tail_calls,
        "scope": {
            "complete_selected_body_pin": True, "complete_caller_triplet_pin": True,
            "direct_descriptor_admission": True, "descriptor_contents_validated": False,
            "device_visibility_ordering": False, "host_rx_dma_completion": False,
            "host_buffer_lifetime": False, "mfd_feed_completion": False,
            "scaler_capture_completion": False, "picture_source_ownership": False,
            "opaque_callee_semantics": False,
        },
        "assumptions": [
            "The 32-byte record remains readable and stable while the selected body rereads its low word.",
            "UART-character/log callees return and preserve the A32 callee-saved ABI; their internal effects are not modeled.",
            "Direct CPU store order is not a device-visibility, DMA-completion or host-notification receipt.",
        ],
    }


def _channel_field_map(payload):
    """Pinned stock A32 channel-field sites, not whole-image alias recovery."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("channel-field payload size does not match the bundled baseline")
    regions = _CHANNEL_FIELD_REGIONS
    total = sum(size for _, _, size, _ in regions)
    dependency_regions = _RX_DESCRIPTOR_ADMISSION_REGIONS
    dependency_total = sum(size for _, _, size, _ in dependency_regions)
    caller_scan_roles = ("helper_15d8_direct_caller", "helper_12e4_direct_caller")
    caller_scan_regions = tuple((role, offset, size) for role, offset, size, _ in regions
                                if role in caller_scan_roles)
    if tuple(role for role, _, _ in caller_scan_regions) != caller_scan_roles:
        raise FormatError("channel-field caller scan regions do not match the baseline")
    if len(regions) > MAX_CHANNEL_FIELD_REGIONS or total > MAX_CHANNEL_FIELD_BYTES:
        raise FormatError("channel-field validation budget exceeded")
    if (len(regions) + len(dependency_regions) > MAX_CHANNEL_FIELD_AGGREGATE_REGIONS or
            total + dependency_total > MAX_CHANNEL_FIELD_AGGREGATE_BYTES):
        raise FormatError("channel-field aggregate validation budget exceeded")
    validated = []
    # Gate every enclosing envelope before decoding an instruction or invoking
    # the separately pinned RX-admission proof.
    for role, offset, size, expected in regions:
        data = bounded(payload, offset, size, "channel-field region")
        if hashlib.sha256(data).hexdigest() != expected:
            raise FormatError(f"channel-field region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset,
                          "size": size, "sha256": expected})

    admission = _rx_descriptor_admission_map(payload)
    branch_candidates = _a32_branch_candidates_in_regions(payload, caller_scan_regions)
    link_candidates = [record for record in branch_candidates if record["operation"] == "BL"]
    expected_links = (
        (0x4a78, 0x898), (0x4acc, 0x203c4), (0x4b00, 0x15d8),
        (0x4ca0, 0x898), (0x4cfc, 0x203c4), (0x4d1c, 0xaf18),
        (0x4d7c, 0xaf18), (0x4dbc, 0x70f0), (0x4dd4, 0x710c),
        (0x4ddc, 0xaf18), (0x4dec, 0x12e4), (0x4dfc, 0xaf5c),
        (0x4e54, 0x203c4),
    )
    if (len(branch_candidates) != 38 or
            tuple((record["blob_file_offset"], record["target_blob_file_offset"])
                  for record in link_candidates) != expected_links):
        raise FormatError("channel-field caller branch candidates do not match the baseline")

    def checked_sites(entries, access, expression):
        result = []
        for entry in entries:
            offset, expected = entry[:2]
            word = _bootstrap_word(payload, offset)
            if word != expected:
                raise FormatError(f"channel-field word at {offset:#x} does not match the baseline")
            record = {"blob_file_offset": offset, "word": word,
                      "access": access, "address_expression": expression}
            if len(entry) == 3:
                record.update(entry[2])
            result.append(record)
        return result

    def checked_anchors(entries):
        result = []
        for role, offset, expected, expression in entries:
            word = _bootstrap_word(payload, offset)
            if word != expected:
                raise FormatError(f"channel-field anchor at {offset:#x} does not match the baseline")
            result.append({"role": role, "blob_file_offset": offset,
                           "word": word, "expression": expression})
        return result

    root = _a32_literal(payload, 0x898)
    init_root = _a32_literal(payload, 0x56c)
    reinit_root = _a32_literal(payload, 0x89d4)
    host_start_global = _a32_literal(payload, 0x465c)
    device_start_global = _a32_literal(payload, 0x5ce8)
    device_start_output = _a32_literal(payload, 0x5d44)
    if ((root["literal_blob_file_offset"], root["literal_value"],
         root["destination_register"]) != (0x6fc, 0xd3a00, 0) or
            (init_root["literal_blob_file_offset"], init_root["literal_value"],
             init_root["destination_register"]) != (0x6fc, 0xd3a00, 4) or
            (reinit_root["literal_blob_file_offset"], reinit_root["literal_value"],
             reinit_root["destination_register"]) != (0x8c24, 0xd3a00, 0) or
            (host_start_global["literal_blob_file_offset"], host_start_global["literal_value"],
             host_start_global["destination_register"]) != (0x3de8, 0xd1ff4, 10) or
            (device_start_global["literal_blob_file_offset"], device_start_global["literal_value"],
             device_start_global["destination_register"]) != (0x5128, 0xd1ff4, 7) or
            (device_start_output["literal_blob_file_offset"], device_start_output["literal_value"],
             device_start_output["destination_register"]) != (0x5ed4, 0xd1ff8, 1)):
        raise FormatError("channel-field root literal does not match the baseline")

    anchors = checked_anchors((
        ("root_return", 0x89c, 0xe12fff1e, "return 0xd3a00"),
        ("slot_stride_words", 0x908, 0xe3a00073, "0x73 words"),
        ("slot_stride_multiply", 0x914, 0xe0000097, "slot * 0x73"),
        ("slot_base", 0x918, 0xe0865100, "C = root + slot * 0x1cc"),
        ("picture_slot_stride_words", 0x837c, 0xe3a00073, "0x73 words"),
        ("picture_slot_stride_multiply", 0x8380, 0xe0000099, "slot * 0x73"),
        ("picture_slot_base", 0x8384, 0xe0884100, "C = root + slot * 0x1cc"),
        ("W_open", 0xb0c, 0xe2851018, "W = C + 0x18"),
        ("W_picture", 0x83b8, 0xe2845018, "W = C + 0x18"),
        ("W_close", 0xa4c4, 0xe2804018, "W = C + 0x18"),
        ("W_scan_first", 0xa5e0, 0xe2805018, "W = C + 0x18"),
        ("W_scan_second", 0xa660, 0xe2805018, "W = C + 0x18"),
        ("X_open", 0x970, 0xe28520ac, "X = C + 0xac"),
        ("X_config", 0xacc, 0xe28530ac, "X = C + 0xac"),
        ("X_close", 0xc18, 0xe28610ac, "X = C + 0xac"),
        ("K_reuse_bit100", 0x841c, 0xe28410ec, "K = C + 0xec"),
        ("K_reuse_normal", 0x84c0, 0xe28410ec, "K = C + 0xec"),
        ("K_publish_bit100", 0x86cc, 0xe28400ec, "K = C + 0xec"),
        ("K_publish_normal", 0x8744, 0xe28400ec, "K = C + 0xec"),
        ("D_delivery", 0x7778, 0xe2840f62, "D = C + 0x188"),
        ("D_consumer", 0x8818, 0xe2840f62, "D = C + 0x188"),
        ("init_save_output_pointer", 0x558, 0xe1a07001, "r7 = init output pointer"),
        ("init_publish_context", 0x880, 0xe5874000, "*0xd1ff8 = 0xd3a00"),
        ("host_start_context_load", 0x466c, 0xe59a0004, "C root = *(0xd1ff4 + 4)"),
        ("decoder_open_output_argument", 0xa308, 0xe2841008,
         "r1 = &W[2] == &C+0x20"),
        ("decoder_open_output_saved", 0xf7f0, 0xe1a0a001,
         "r10 = decoder output pointer"),
        ("xpt_playback_output_argument", 0x93a8, 0xe2841010,
         "r1 = &X[4] == &C+0xbc"),
        ("xpt_playback_output_saved", 0x1bc44, 0xe1a09001,
         "r9 = playback output pointer"),
        ("xpt_slot_channel_scale", 0x1bc90, 0xe0860106,
         "r0 = channel * 5"),
        ("xpt_slot_offset", 0x1bc94, 0xe3001828,
         "r1 = 0x828"),
        ("xpt_slot_controller_base", 0x1bc98, 0xe0811005,
         "r1 = controller + 0x828"),
        ("xpt_slot_channel_address", 0x1bc9c, 0xe0814180,
         "r4 = controller + 0x828 + channel * 0x28"),
        ("pvr_open_output_argument", 0x5960, 0xe28700d4,
         "r0 = &C+0xd4"),
        ("pvr_open_output_recovered", 0xc02c, 0xe59d0034,
         "r0 = caller output pointer"),
        ("helper_15d8_caller_entry", 0x4a60, 0xe92d41f0,
         "complete selected caller entry"),
        ("helper_15d8_root_save", 0x4a7c, 0xe1a02000,
         "r2 = fixed-root getter result"),
        ("helper_15d8_slot_argument", 0x4a90, 0xe5940008,
         "r0 = selected channel slot"),
        ("helper_15d8_second_argument", 0x4afc, 0xe594100c,
         "r1 = selected caller value"),
        ("helper_12e4_caller_entry", 0x4c7c, 0xe92d5ff0,
         "complete selected caller entry; r6 is saved"),
        ("helper_12e4_root_save", 0x4ca4, 0xe1a06000,
         "r6 = fixed-root getter result"),
        ("helper_12e4_slot_argument", 0x4de0, 0xe5940008,
         "r0 = selected channel slot"),
        ("helper_12e4_root_argument", 0x4de4, 0xe1a01006,
         "r1 = saved fixed root"),
        ("helper_12e4_third_argument", 0x4de8, 0xe594200c,
         "r2 = selected caller value"),
    ))

    anchor_by_role = {record["role"]: record for record in anchors}
    caller_edges = {}
    for role, site, target in (
            ("helper_15d8_root_getter", 0x4a78, 0x898),
            ("helper_15d8_call", 0x4b00, 0x15d8),
            ("helper_12e4_root_getter", 0x4ca0, 0x898),
            ("helper_12e4_call", 0x4dec, 0x12e4)):
        branch = _a32_branch(payload, site, link=True)
        if branch["target_blob_file_offset"] != target:
            raise FormatError(f"channel-field caller edge {role} does not match the baseline")
        caller_edges[role] = {"role": role, **branch}

    selected_fixed_root_paths = (
        {
            "helper_function_entry": 0x15d8,
            "incoming_root_register": "r2",
            "caller_function_entry": 0x4a60,
            "caller_region_role": "helper_15d8_direct_caller",
            "root_value": 0xd3a00,
            "root_getter_call": caller_edges["helper_15d8_root_getter"],
            "root_save": anchor_by_role["helper_15d8_root_save"],
            "helper_call": caller_edges["helper_15d8_call"],
            "helper_root_argument": anchor_by_role["helper_15d8_root_save"],
            "possible_intervening_link_candidate_sites": [],
            "fixed_root_on_this_selected_path": True,
            "callee_saved_register_premise": False,
            "runtime_path_observed": False,
            "all_direct_callers_established": False,
            "indirect_or_computed_callers_excluded": False,
        },
        {
            "helper_function_entry": 0x12e4,
            "incoming_root_register": "r1",
            "caller_function_entry": 0x4c7c,
            "caller_region_role": "helper_12e4_direct_caller",
            "root_value": 0xd3a00,
            "root_getter_call": caller_edges["helper_12e4_root_getter"],
            "root_save": anchor_by_role["helper_12e4_root_save"],
            "helper_call": caller_edges["helper_12e4_call"],
            "helper_root_argument": anchor_by_role["helper_12e4_root_argument"],
            "possible_intervening_link_candidate_sites": [
                0x4d1c, 0x4d7c, 0x4dbc, 0x4dd4, 0x4ddc,
            ],
            "fixed_root_on_this_selected_path": True,
            "callee_saved_register_premise": True,
            "runtime_path_observed": False,
            "all_direct_callers_established": False,
            "indirect_or_computed_callers_excluded": False,
        },
    )
    fixed_path_by_helper = {
        record["helper_function_entry"]: record for record in selected_fixed_root_paths
    }

    # Schema v1 compatibility: retain the legacy selected-caller boolean while
    # the new fields distinguish selected evidence from complete provenance.
    argument_conditional_aliases = (
        {
            "function_entry": 0x12e4,
            "incoming_root_register": "r1",
            "premise": "incoming r1 == channel root 0xd3a00",
            "fixed_root_caller_provenance_pinned": True,
            "selected_fixed_root_caller_provenance_pinned": True,
            "selected_fixed_root_call_paths": [fixed_path_by_helper[0x12e4]],
            "all_caller_provenance_pinned": False,
            "derivation_sites": checked_anchors((
                ("helper_12e4_stride_words", 0x12e8, 0xe3a03073,
                 "r3 = 0x73 words"),
                ("helper_12e4_stride_multiply", 0x12ec, 0xe0000390,
                 "r0 = slot * 0x73"),
                ("helper_12e4_channel_base", 0x12f0, 0xe0814100,
                 "r4 = incoming r1 + slot * 0x1cc"),
            )),
            "access_sites": [0x12fc, 0x1344, 0x1354, 0x13f4, 0x1478],
        },
        {
            "function_entry": 0x15d8,
            "incoming_root_register": "r2",
            "premise": "incoming r2 == channel root 0xd3a00",
            "fixed_root_caller_provenance_pinned": True,
            "selected_fixed_root_caller_provenance_pinned": True,
            "selected_fixed_root_call_paths": [fixed_path_by_helper[0x15d8]],
            "all_caller_provenance_pinned": False,
            "derivation_sites": checked_anchors((
                ("helper_15d8_stride_words", 0x15dc, 0xe3a03073,
                 "r3 = 0x73 words"),
                ("helper_15d8_stride_multiply", 0x15e4, 0xe0000390,
                 "r0 = slot * 0x73"),
                ("helper_15d8_channel_base", 0x15e8, 0xe0820100,
                 "r0 = incoming r2 + slot * 0x1cc"),
            )),
            "access_sites": [0x15ec],
        },
        {
            "function_entry": 0x1610,
            "incoming_root_register": "r1",
            "premise": "incoming r1 == channel root 0xd3a00",
            "fixed_root_caller_provenance_pinned": False,
            "selected_fixed_root_caller_provenance_pinned": False,
            "selected_fixed_root_call_paths": [],
            "all_caller_provenance_pinned": False,
            "derivation_sites": checked_anchors((
                ("helper_1610_stride_words", 0x1618, 0xe3a02073,
                 "r2 = 0x73 words"),
                ("helper_1610_stride_multiply", 0x161c, 0xe0000290,
                 "r0 = slot * 0x73"),
                ("helper_1610_channel_base", 0x1620, 0xe0814100,
                 "r4 = incoming r1 + slot * 0x1cc"),
            )),
            "access_sites": [0x1628, 0x1664],
        },
    )

    branch_specs = (
        ("global_clear", 0x568, 0x206e4),
        ("reinitialize_clear", 0x89e0, 0x206e4),
        ("open_clear", 0x5288, 0x206e4),
        ("host_interface_init", 0x5d4c, 0x54c),
        ("decoder_wrapper_open", 0xb18, 0xa2a4),
        ("decoder_object_open", 0xa318, 0xf7e4),
        ("xpt_context_open", 0x980, 0x9300),
        ("xpt_context_configure", 0xad8, 0x973c),
        ("xpt_context_close", 0xc1c, 0xa158),
        ("pvr_play_open", 0x596c, 0xbddc),
        ("xpt_playback_open", 0x93b0, 0x1bc3c),
        ("cached_record_producer_null", 0x8480, 0xe110),
        ("cached_record_producer_selected", 0x85f8, 0xe110),
        ("cache_reuse_bit100", 0x8424, 0x20708),
        ("cache_reuse_normal", 0x84c8, 0x20708),
        ("cache_publish_bit100", 0x86d8, 0x20708),
        ("cache_publish_normal", 0x8750, 0x20708),
        ("descriptor_delivery_copy", 0x777c, 0x2c59c),
        ("descriptor_consumer", 0x881c, 0x77e0),
        ("copy_wrapper", 0x20724, 0x2c59c),
    )
    control_flow = []
    for role, site, target in branch_specs:
        branch = _a32_branch(payload, site, link=True)
        if branch["target_blob_file_offset"] != target:
            raise FormatError(f"channel-field branch {role} does not match the baseline")
        control_flow.append({"role": role, **branch})

    decoder_reads = checked_sites((
        (0xb7c, 0xe893000e, {"operation": "LDM includes W+0x8"}),
        (0xfe4, 0xe5941008), (0xff0, 0xe5940008),
        (0x10ac, 0xe5940008), (0x10e8, 0xe5940008),
        (0x1288, 0xe5940008),
        (0x1344, 0xe5960008,
         {"alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x12e4}),
        (0x13f4, 0xe5960008,
         {"alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x12e4}),
        (0x1478, 0xe5960008,
         {"alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x12e4}),
        (0x8454, 0xe5950008), (0x8474, 0xe5950008),
        (0x85f4, 0xe5950008), (0x8938, 0xe5950008),
        (0xa394, 0xe5941008), (0xa4c8, 0xe5940008),
        (0xa5e8, 0xe5950008), (0xa600, 0xe5950008),
        (0xa668, 0xe5950008), (0xa680, 0xe5950008),
    ), "read", "W + 0x8 == C + 0x20")
    decoder_reads += checked_sites((
        (0x15ec, 0xe5900020,
         {"alias_provenance": "conditional",
          "premise": "incoming r2 == channel root 0xd3a00",
          "function_entry": 0x15d8}),
        (0x1628, 0xe5940020,
         {"alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x1610}),
        (0x1664, 0xe5940020,
         {"alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x1610}),
        (0x1704, 0xe5940020),
        (0x171c, 0xe5940020), (0x1780, 0xe5940020),
        (0x1790, 0xe5940020), (0xa56c, 0xe5940020),
        (0xa5a0, 0xe5940020),
    ), "read", "C + 0x20")
    decoder_reads += checked_sites(((0xfbc4, 0xe59a0000),), "read", "*out == C + 0x20")
    decoder_reads.sort(key=lambda record: record["blob_file_offset"])
    decoder_writes = checked_sites((
        (0xf83c, 0xe58a0000, {"value": 0}),
        (0xfbb0, 0xe58a4000, {"value": "allocated BXVD channel object"}),
    ), "write", "*out == C + 0x20")

    playback_reads = checked_sites((
        (0x48cc, 0xe59000bc), (0x48e0, 0xe59000bc),
        (0x5964, 0xe59720bc), (0x6d40, 0xe59000bc),
    ), "read", "C + 0xbc")
    playback_reads += checked_sites((
        (0x941c, 0xe5941010), (0x9438, 0xe5940010),
        (0x945c, 0xe5940010), (0x95d0, 0xe5940010),
        (0x9618, 0xe5940010), (0x99bc, 0xe5970010),
        (0x9a34, 0xe5970010), (0xa164, 0xe5940010),
    ), "read", "X + 0x10 == C + 0xbc")
    playback_reads.sort(key=lambda record: record["blob_file_offset"])
    playback_writes = checked_sites((
        (0xa180, 0xe5846010, {"value": 0}),
        (0x1bd7c, 0xe5894000,
         {"value": "controller + 0x828 + channel * 0x28"}),
    ), "write", "X + 0x10 == C + 0xbc")

    pvr_reads = checked_sites((
        (0x12fc, 0xe59480d4,
         {"alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x12e4}),
        (0x45b0, 0xe59500d4),
        (0x45ec, 0xe59500d4), (0x4888, 0xe59000d4),
        (0x504c, 0xe59000d4), (0x59ac, 0xe59000d4),
        (0x6b9c, 0xe59000d4), (0x6c40, 0xe59000d4),
        (0x6ca8, 0xe59000d4), (0x6d18, 0xe59000d4),
        (0x6d8c, 0xe59000d4),
    ), "read", "C + 0xd4")
    pvr_writes = checked_sites((
        (0x5064, 0xe58090d4, {"value": 0}),
        (0xc030, 0xe5804000, {"value": "PVR play object or zero on failure"}),
    ), "write", "C + 0xd4")

    producer_nonnull = checked_sites((
        (0xe19c, 0xe5950004), (0xe1a0, 0xe5840034),
        (0xe1a4, 0xe5950008), (0xe1a8, 0xe5840038),
    ), "read/write projection", "metadata + 4/+8 -> P + 0x34/+0x38")
    producer_null = checked_sites((
        (0xe210, 0xe5840034, {"value": 0}),
        (0xe214, 0xe5840038, {"value": 0}),
    ), "write", "P + 0x34/+0x38")
    cache_transfers = []
    for direction, sites, source, destination in (
            ("channel_to_stack", ((0x8418, 0xe3a0208c), (0x841c, 0xe28410ec),
                                  (0x8420, 0xe28d0020), (0x8424, 0xeb0060b7)),
             "[C+0xec,C+0x178)", "[P,P+0x8c)"),
            ("channel_to_stack", ((0x84bc, 0xe3a0208c), (0x84c0, 0xe28410ec),
                                  (0x84c4, 0xe28d0020), (0x84c8, 0xeb00608e)),
             "[C+0xec,C+0x178)", "[P,P+0x8c)"),
            ("stack_to_channel", ((0x86cc, 0xe28400ec), (0x86d0, 0xe3a0208c),
                                  (0x86d4, 0xe28d1020), (0x86d8, 0xeb00600a)),
             "[P,P+0x8c)", "[C+0xec,C+0x178)"),
            ("stack_to_channel", ((0x8744, 0xe28400ec), (0x8748, 0xe3a0208c),
                                  (0x874c, 0xe28d1020), (0x8750, 0xeb005fec)),
             "[P,P+0x8c)", "[C+0xec,C+0x178)")):
        cache_transfers.append({
            "direction": direction, "source": source, "destination": destination,
            "bytes": 140,
            "sites": checked_sites(sites, "copy setup/call", source + " -> " + destination),
        })

    flag178_reads = checked_sites(((0x84a0, 0xe5d40178),), "read", "C + 0x178")
    flag178_writes = checked_sites((
        (0x86ac, 0xe5c45178, {"value": 1}),
        (0x873c, 0xe5c47178, {"value": 0}),
        (0x8814, 0xe5c45178, {"value": 1}),
    ), "write", "C + 0x178")
    flag180_reads = checked_sites((
        (0x83bc, 0xe5d40180), (0x88d4, 0xe5d40180),
        (0xa5bc, 0xe5d40180), (0xa634, 0xe5d40180),
    ), "read", "C + 0x180")
    flag180_writes = checked_sites((
        (0x1354, 0xe5c40180,
         {"value": 1, "alias_provenance": "conditional",
          "premise": "incoming r1 == channel root 0xd3a00",
          "function_entry": 0x12e4}),
        (0x86a8, 0xe5c45180, {"value": 1}),
        (0x8738, 0xe5c47180, {"value": 0}),
        (0x87e4, 0xe5c45180, {"value": 1}),
        (0x87fc, 0xe5c45180, {"value": 1}),
        (0x8808, 0xe5c47180, {"value": 0}),
        (0x8810, 0xe5c45180, {"value": 1}),
    ), "write", "C + 0x180")

    descriptor_reads = []
    for expression, entries in (
            ("D + 0", ((0x77f0, 0xe5940000),)),
            ("D + 4 and D + 8", ((0x7800, 0xe9940006), (0x784c, 0xe9940006))),
            ("D + 4", ((0x7814, 0xe5940004), (0x7860, 0xe5940004),
                       (0x7874, 0xe5940004))),
            ("D + 8", ((0x780c, 0xe5940008), (0x7858, 0xe5940008)))):
        descriptor_reads += checked_sites(entries, "read", expression)
    descriptor_reads.sort(key=lambda record: record["blob_file_offset"])
    descriptor_write = checked_sites((
        (0x777c, 0xeb009386, {"bytes": 32, "operation": "BL memcpy"}),
    ), "range write", "[D,D+0x20)")

    initialization = {
        "root_clear": {
            "destination": 0xd3a00, "bytes": 0x74c,
            "four_slot_bytes": 4 * 0x1cc, "tail_bytes": 0x1c,
            "sites": checked_sites(((0x55c, 0xe59f0198), (0x560, 0xe300274c),
                                    (0x564, 0xe3a01000), (0x568, 0xeb00805d)),
                                   "range write", "[0xd3a00,0xd414c)"),
        },
        "reinitialize_clear": {
            "destination": 0xd3a00, "bytes": 0x74c,
            "four_slot_bytes": 4 * 0x1cc, "tail_bytes": 0x1c,
            "sites": checked_sites(((0x89d4, 0xe59f0248), (0x89d8, 0xe300274c),
                                    (0x89dc, 0xe3a01000), (0x89e0, 0xeb005f3f)),
                                   "range write", "[0xd3a00,0xd414c)"),
        },
        "open_clear": {
            "destination_expression": "C + 0x10", "bytes": 0x1cc,
            "nominal_slot_end_expression": "C + 0x1cc",
            "range_end_expression": "C + 0x1dc",
            "crosses_nominal_slot_end_by_bytes": 0x10,
            "sites": checked_sites(((0x5270, 0xe30021cc), (0x5274, 0xe3a01000),
                                    (0x5278, 0xe0050097), (0x527c, 0xe59b0004),
                                    (0x5280, 0xe0800105), (0x5284, 0xe2800010),
                                    (0x5288, 0xeb006d15)),
                                   "range write", "[C+0x10,C+0x1dc)"),
        },
        "fill_helper": {"a32_wrapper": 0x206e4, "helper_isa": "Thumb",
                        "thumb_value_entry": 0x2c688,
                        "thumb_fill_entry": 0x2c73c, "fill_byte": 0},
    }

    bulk_write_paths = ["root_clear", "reinitialize_clear", "open_clear"]
    fields = {
        "0x20": {"offset": 0x20, "width_bytes": 4,
                 "classification": "BXVD decoder channel handle",
                 "selected_scalar_reads": decoder_reads,
                 "selected_scalar_writes": decoder_writes,
                 "bulk_range_write_paths": list(bulk_write_paths),
                 "selected_accesses_complete": False},
        "0xbc": {"offset": 0xbc, "width_bytes": 4,
                 "classification": "XPT playback channel slot handle",
                 "selected_scalar_reads": playback_reads,
                 "selected_scalar_writes": playback_writes,
                 "bulk_range_write_paths": list(bulk_write_paths),
                 "selected_accesses_complete": False,
                 "slot_expression": "controller + 0x828 + channel * 0x28",
                 "separate_allocation_established": False,
                 "active_RAVE_context_identity_established": False},
        "0xd4": {"offset": 0xd4, "width_bytes": 4,
                 "classification": "PVR play object",
                 "selected_scalar_reads": pvr_reads,
                 "selected_scalar_writes": pvr_writes,
                 "bulk_range_write_paths": list(bulk_write_paths),
                 "selected_accesses_complete": False,
                 "BXVD_decoder_handle": False},
        "0x120": {"offset": 0x120, "width_bytes": 4,
                  "classification": "cached record word at K+0x34 (P+0x34 transfer position)",
                  "selected_direct_scalar_accesses": [],
                  "bulk_range_write_paths": list(bulk_write_paths),
                  "selected_accesses_complete": False,
                  "projection": "cached_metadata_words"},
        "0x124": {"offset": 0x124, "width_bytes": 4,
                  "classification": "cached record word at K+0x38 (P+0x38 transfer position)",
                  "selected_direct_scalar_accesses": [],
                  "bulk_range_write_paths": list(bulk_write_paths),
                  "selected_accesses_complete": False,
                  "projection": "cached_metadata_words"},
        "0x178": {"offset": 0x178, "width_bytes": 1,
                  "classification": "picture-cache fresh/reuse gate",
                  "selected_scalar_reads": flag178_reads,
                  "selected_scalar_writes": flag178_writes,
                  "bulk_range_write_paths": list(bulk_write_paths),
                  "selected_accesses_complete": False,
                  "ownership_flag_established": False},
        "0x180": {"offset": 0x180, "width_bytes": 1,
                  "classification": "picture refresh/return-path gate",
                  "selected_scalar_reads": flag180_reads,
                  "selected_scalar_writes": flag180_writes,
                  "bulk_range_write_paths": list(bulk_write_paths),
                  "selected_accesses_complete": False,
                  "ownership_flag_established": False},
        "0x188": {"offset": 0x188, "width_bytes": 32,
                  "classification": "host Y-RX descriptor record",
                  "selected_range_writes": descriptor_write,
                  "selected_scalar_reads": descriptor_reads,
                  "bulk_range_write_paths": list(bulk_write_paths),
                  "selected_accesses_complete": False,
                  "bytes_read_by_admission": 12, "remaining_bytes_read_by_admission": False,
                  "producer_source_proven": False,
                  "admission": admission},
    }
    tracked_helper_candidates = [
        record for record in link_candidates
        if record["target_blob_file_offset"] in (0x12e4, 0x15d8, 0x1610)
    ]
    return {
        "schema_version": 1, "kind": "stock-arm-channel-field-inventory",
        "isa": "A32", "endianness": "little",
        "device_observed": False,
        "validation": {"region_count": len(validated), "bytes": total,
                       "regions": validated,
                       "dependency_region_count": len(dependency_regions),
                       "dependency_bytes": dependency_total,
                       "aggregate_region_count": len(validated) + len(dependency_regions),
                       "aggregate_bytes_charged": total + dependency_total,
                       "aggregate_overlap_deduplicated": False,
                       "caller_scan_region_count": len(caller_scan_regions),
                       "caller_scan_bytes": sum(size for _, _, size in caller_scan_regions),
                       "rx_descriptor_admission": admission["validation"]},
        "channel": {
            "root": root, "init_root": init_root, "reinitialize_root": reinit_root,
            "slot_count": 4, "slot_stride_words": 0x73,
            "slot_stride_bytes": 0x1cc,
            "channel_expression": "C = 0xd3a00 + slot * 0x1cc",
            "derivation_sites": anchors,
            "aliases": {"W": "C + 0x18", "X": "C + 0xac",
                        "K": "C + 0xec", "D": "C + 0x188", "P": "sp + 0x20"},
            "host_interface_handoff": {
                "scope": "on successful init",
                "global_address": 0xd1ff4, "context_output_address": 0xd1ff8,
                "host_start_global": host_start_global,
                "device_start_global": device_start_global,
                "device_start_output": device_start_output,
            },
        },
        "initialization": initialization,
        "access_inventory": {
            "kind": "selected pinned CPU access sites plus explicit bulk clear paths",
            "bulk_range_write_paths": list(bulk_write_paths),
            "per_field_access_inventory_complete": False,
        },
        "fields": fields,
        "projections": {
            "cached_metadata_words": {
                "producer_nonnull": producer_nonnull, "producer_null": producer_null,
                "cache_range": "[C+0xec,C+0x178)", "cache_bytes": 140,
                "cache_address_projection": [
                    "C+0x120 == K+0x34",
                    "C+0x124 == K+0x38",
                ],
                "nonnull_fresh_path_writes": [
                    "P+0x34 <- metadata+4",
                    "P+0x38 <- metadata+8",
                ],
                "null_path_writes": ["P+0x34 <- 0", "P+0x38 <- 0"],
                "reuse_path": "existing [C+0xec,C+0x178) is copied to P",
                "transfers": cache_transfers,
                "range_end_excludes_flag_0x178": True,
                "intervening_callee_preservation_verified": False,
                "published_value_equivalence_established": False,
                "cache_value_currentness_established": False,
            },
        },
        "caller_provenance": {
            "kind": "selected fixed-root paths in two hash-pinned A32 caller bodies",
            "selected_fixed_root_paths": list(selected_fixed_root_paths),
            "branch_candidate_scan": {
                "isa_encoding": "A32 conditional-space B/BL immediate bit patterns",
                "region_count": len(caller_scan_regions),
                "bytes": sum(size for _, _, size in caller_scan_regions),
                "regions": [
                    {"role": role, "blob_file_offset": offset, "size": size}
                    for role, offset, size in caller_scan_regions
                ],
                "alignment_bytes": 4,
                "branch_immediate_candidate_count": len(branch_candidates),
                "link_candidate_count": len(link_candidates),
                "link_candidates": link_candidates,
                "tracked_helper_entries": [0x12e4, 0x15d8, 0x1610],
                "tracked_helper_link_candidates": tracked_helper_candidates,
                "complete_for_pinned_region_encoding_candidates": True,
                "whole_image_scan": False,
                "all_direct_callers_established": False,
                "indirect_or_computed_callers_excluded": False,
            },
            "limitations": [
                "The scan covers only the two named pinned caller bodies; other direct callers are not excluded.",
                "No direct candidate for 0x1610 in those bodies does not exclude a caller elsewhere or an indirect/computed call.",
                "The 0x12e4 path relies on A32 callee-saved r6 preservation across its possible intervening calls.",
                "Static selected paths do not establish runtime execution, valid slot range, object identity or lifetime.",
            ],
        },
        "argument_conditional_aliases": list(argument_conditional_aliases),
        "control_flow": control_flow,
        "excluded_lookalikes": [
            {"blob_file_offsets": [0xf96c, 0xf97c], "encoded_offset": 0xd4,
             "reason": "base is the allocated BXVD object, not C"},
            {"encoded_offset": 0x120,
             "reason": "other base registers with the same immediate are outside the proved C aliases"},
        ],
        "scope": {
            "all_decoded_instruction_and_literal_offsets_inside_pinned_regions": True,
            "all_reported_branch_targets_inside_pinned_regions": False,
            "fixed_root_derived_aliases_are_pinned": True,
            "selected_fixed_root_caller_paths_are_pinned": True,
            "direct_caller_inventory_complete": False,
            "indirect_or_computed_caller_inventory_complete": False,
            "all_listed_accesses_have_fixed_root_provenance": False,
            "argument_rooted_aliases_are_conditional": True,
            "per_field_access_inventory_complete": False,
            "complete_function_envelopes": False,
            "whole_image_instruction_scan": False,
            "complete_firmware_alias_recovery": False,
            "opaque_callee_aliases_complete": False,
            "arc_or_dma_writers_complete": False,
            "runtime_object_identity": False,
            "source_address_units": False,
            "source_allocation_extent": False,
            "source_plane_lease": False,
            "flags_form_ownership_contract": False,
            "descriptor_producer_source_proven": False,
            "descriptor_dma_completion": False,
        },
        "assumptions": [
            "A32 calls use the pinned calling convention and output pointers remain unaliased for each serialized call.",
            "Two selected direct caller paths establish the stated C-root premise; other direct or indirect callers remain unclassified.",
            "The third argument-rooted helper at 0x1610 remains conditional on its incoming C-root premise.",
            "Selected scalar access lists exclude unpinned aliases and are not whole-image access inventories; the three pinned bulk clear paths are listed separately.",
            "Reported branch targets are decoded from pinned call instructions; target bodies are not thereby claimed as pinned.",
            "The generic clear and copy helpers operate on the exact argument ranges shown by their pinned wrappers and bodies.",
            "After successful init, host interface +4 retains its 0xd3a00 output while serialized START reads it.",
            "Static handle and field roles do not establish live runtime ownership, address units, allocation extent or completion.",
        ],
    }


_PPB_BANK_RELEASE_EDGES = (
    ("returned", "Core_Run", 2, 0x51a4, 0x51a8, 0x3000),
    ("latest", "Core_OrderPIF_ReleaseOnLatest", 4, 0x9280, 0x9284, 0x4000),
    ("attempt", "AttemptRelease", 4, 0xabcc, 0xabd0, 0x4000),
    ("discard", "Core_AttemptDisplay", 4, 0xb964, 0xb968, 0x3000),
    ("no_display", "Core_SetPIF_NoDisplay", 16, 0x25f90, 0x25f94, 0x2000),
    ("release", "Core_ReleasePPB", 16, 0x26130, 0x2612c, 0x6000),
    ("undelivered_display", "Core_GetUndeliveredPPBs", 16, 0x261d4, 0x261d0, 0x6000),
    ("undelivered_return", "Core_GetUndeliveredPPBs", 16, 0x26264, 0x26260, 0x6000),
    ("late", "Core_Late_PPB_Release", 16, 0x26390, 0x2638c, 0x3000),
)


def _ppb_bank_u32(value, label="scalar"):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise FormatError(f"PPB bank {label} must be a u32")
    return value


def _ppb_bank_capacity(dividend, divisor):
    """Actual selected unsigned division followed by a SIGNED GE clamp."""
    dividend, divisor = _ppb_bank_u32(dividend), _ppb_bank_u32(divisor)
    quotient, remainder = divmod(dividend, divisor) if divisor else (0x7fffffff, 0)
    signed = quotient if quotient < 0x80000000 else quotient - (1 << 32)
    return {"quotient": quotient, "remainder": remainder,
            "capacity": 32 if signed >= 32 else quotient, "division_by_zero": divisor == 0}


def _ppb_bank_geometry(width, height, stripe_exponent, alignment_mask, metadata_extra=False):
    """Pinned equations, conditional on ARC flags and vendor mul16 semantics.

    This deliberately refuses unknown shift/mul16 domains rather than inventing
    silicon behavior. Refusal is a model limitation, not a firmware input guard.
    """
    width, height = _ppb_bank_u32(width, "width"), _ppb_bank_u32(height, "height")
    if type(stripe_exponent) is not int or not 0 <= stripe_exponent < 32:
        raise FormatError("PPB bank stripe shift is outside the supported ISA domain")
    if type(alignment_mask) is not int or not 0 <= alignment_mask <= 255 or type(metadata_extra) is not bool:
        raise FormatError("PPB bank geometry requires byte alignment mask and boolean metadata flag")
    u32 = lambda value: value & 0xffffffff
    signed = lambda value: value if value < 0x80000000 else value - (1 << 32)
    page = lambda value: u32(value + 4095) & 0xfffff000
    stripe = 1 << stripe_exponent

    def striped(value):
        rounded = u32(value + stripe - 1) & u32(~(stripe - 1))
        if not ((rounded >> stripe_exponent) & 1):
            rounded = u32(rounded + stripe)
        return min(rounded, 1120) & 0xffff  # STW then LDW in VideoParameters.

    y_height = striped(height)
    c_height = striped(u32(signed(height) >> 1))
    pitch = u32(width + alignment_mask) & u32(~alignment_mask)
    # Both operands below 32768 avoid signed-vs-unsigned low16 ambiguity, but
    # even this product remains conditional on the unvalidated vendor opcode.
    if pitch >= 32768 or y_height >= 32768 or c_height >= 32768:
        raise FormatError("PPB bank mul16 operands are outside the conditional supported domain")
    y_bytes = page(u32(pitch * y_height))
    chroma_bytes = u32(pitch * c_height)
    total = page(u32(y_bytes + chroma_bytes))
    extra_offset = total if metadata_extra else 0
    extra_bytes = u32(6 * u32((signed(width) >> 4) * (signed(height) >> 4))) if metadata_extra else 0
    if metadata_extra:
        total = page(u32(total + extra_bytes))
    return {"pitch": pitch, "y_stripe_height": y_height, "chroma_stripe_height": c_height,
            "y_bytes": y_bytes, "chroma_bytes": chroma_bytes, "extra_offset": extra_offset,
            "extra_bytes": extra_bytes, "frame_bytes": total, "conditional_vendor_mul16": True}


def _ppb_context_object_window(pointer, kind):
    """Observer-only full-object guard, not proof of a live malloc allocation."""
    pointer = _ppb_bank_u32(pointer, "context object pointer")
    if type(kind) is not str or kind not in _PPB_CONTEXT_OBJECT_BYTES:
        raise FormatError("PPB context object kind must be H, C, Q or M")
    size = _PPB_CONTEXT_OBJECT_BYTES[kind]
    if pointer & 3 or pointer < 0xd53dc or pointer + size > 0x116000:
        raise FormatError("PPB context object is outside the declared small-heap envelope")
    return {"kind": kind, "pointer": pointer, "bytes": size, "end_exclusive": pointer + size,
            "alignment_bytes": 4, "observer_guards_only": True, "native_allocator_proven": False}


def _ppb_saved_context_window(physical, submitted_bytes, video_base, video_bytes):
    """Normalize a supplied declared slice; never follow a pool/plane value."""
    physical, submitted_bytes, video_base, video_bytes = (
        _ppb_bank_u32(value, "saved context input")
        for value in (physical, submitted_bytes, video_base, video_bytes))
    video_end, submitted_end = video_base + video_bytes, physical + submitted_bytes
    skip = (-physical) & 3
    if (not video_bytes or not 0x116068 <= video_base < video_end <= 0x3ffc000 or
            not video_base <= physical < submitted_end <= video_end or submitted_bytes < skip + 0x177cc):
        raise FormatError("PPB saved context is outside the declared video slice or minimum size")
    context, remaining = physical + skip, submitted_bytes - skip
    spans = [{"role": role, "offset": offset, "address": context + offset, "bytes": size}
             for role, offset, size in _PPB_SAVED_SCALARS]
    if remaining < 0x5bc or any(span["offset"] + span["bytes"] > 0x5bc for span in spans):
        raise FormatError("PPB saved scalar span exceeds the saved core")
    return {"physical": physical, "submitted_bytes": submitted_bytes, "video_base": video_base,
            "video_bytes": video_bytes, "video_end_exclusive": video_end, "alignment_skip": skip,
            "context_address": context, "context_bytes": remaining, "context_end_exclusive": submitted_end,
            "saved_core_bytes": 0x5bc, "read_spans": spans, "total_read_bytes": sum(s["bytes"] for s in spans),
            "observed_inputs_only": True, "model_no_native_certification": True,
            "saved_may_be_stale": True, "non_atomic": True}


def _ppb_saved_context_bridge(payload):
    """Bounded static ARM-to-saved-ARC provenance, not active device state."""
    total = sum(size for _, _, size, _ in _PPB_SAVED_REGIONS)
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or len(_PPB_SAVED_REGIONS) > MAX_PPB_SAVED_REGIONS or
            total > MAX_PPB_SAVED_BYTES):
        raise FormatError("PPB saved-context validation size/budget does not match")
    validated = []
    for role, offset, size, digest in _PPB_SAVED_REGIONS:
        if hashlib.sha256(bounded(payload, offset, size, "PPB saved-context region")).hexdigest() != digest:
            raise FormatError(f"PPB saved-context region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": digest})
    # Resolve original symbols by section, address, size and function type; not name alone.
    base = 0x2ea60
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", payload, base)
    if header[2] != 45 or header[6] != 0x4aae0 or header[11:] != (40, 55, 54):
        raise FormatError("PPB saved-context original ELF header does not match")
    sections = [struct.unpack_from("<10I", payload, 0x79540 + i * 40) for i in range(55)]
    names, strings = payload[0x79098:0x7953f], payload[0x67a95:0x69b6f]
    def string(table, offset):
        if offset >= len(table) or table.find(b"\0", offset) < 0:
            raise FormatError("PPB saved-context original string offset does not match")
        return table[offset:table.index(b"\0", offset)].decode("ascii")
    for index, name, address, offset in ((4, ".core_critical_code_picture", 0x7f8c, 0x43d0),
            (5, ".h264_critical_code_picture", 0xc158, 0x859c), (16, ".text", 0x23d74, 0x17ea8)):
        section = sections[index]
        if string(names, section[0]) != name or (section[1], section[3], section[4]) != (1, address, offset):
            raise FormatError("PPB saved-context containing section does not match")
    symbols = [struct.unpack_from("<IIIBBH", payload, p) for p in range(0x69b70, 0x6cfc0, 16)]
    bodies = []
    for name, index, start, end, offset, digest in _PPB_SAVED_ARC_BODIES:
        section = sections[index]
        matches = [i for i, s in enumerate(symbols) if string(strings, s[0]) == name and
                   s[1:3] == (start, end - start) and s[3] & 15 == 2 and s[5] == index]
        if (len(matches) != 1 or not section[3] <= start < end <= section[3] + section[5] or
                base + section[4] + start - section[3] != offset):
            raise FormatError(f"PPB saved-context section-qualified body {name} does not match")
        bodies.append({"name": name, "section_index": index, "symbol_index": matches[0],
                       "elf_virtual_address": start, "size": end - start, "blob_file_offset": offset, "sha256": digest})
    words = ((0x7dc, 0xe2840008), (0xf8cc, 0xe30a084c), (0xf980, 0xe5845064), (0xfbb0, 0xe58a4000),
        (0x25108, 0xe2841e1a), (0x2b440, 0xe30001f4), (0x2b46c, 0xe5849000), (0x2b470, 0xe584b010),
        (0x2b47c, 0xe5847014), (0x2b550, 0xe5cd000d), (0x2910c, 0xe3a00064), (0x291b4, 0xe5840040),
        (0x25f04, 0xe5840224), (0x25fbc, 0xe5801000), (0x72ac, 0xd5384), (0x72b0, 0x116000))
    if any(_bootstrap_word(payload, p) != word for p, word in words):
        raise FormatError("PPB saved-context critical ARM word does not match")
    calls = []
    for site, target in ((0x7e0, 0xe8b0), (0xa318, 0xf7e4), (0xe8e0, 0x20510), (0xf8d0, 0x20510), (0xfa6c, 0x252a4),
            (0xfb74, 0x27480), (0x25110, 0x2b3f0), (0x2b444, 0x20510), (0x2b570, 0x200d8),
            (0x20150, 0x2906c), (0x2017c, 0x294ec), (0x29110, 0x20510),
            (0x2b698, 0x1f5d4), (0x25ed8, 0x2b628), (0x25efc, 0x1fe6c)):
        word = _bootstrap_word(payload, site)
        displacement = word & 0xffffff
        displacement -= (1 << 24) if displacement & (1 << 23) else 0
        if word & 0xff000000 != 0xeb000000 or site + 8 + 4 * displacement != target:
            raise FormatError("PPB saved-context original ARM BL does not match")
        calls.append({"call_blob_file_offset": site, "target_blob_file_offset": target})
    arc_calls = []
    for index, site, target, delay in ((16, 0x24a14, 0x3b304, False), (16, 0x24aa4, 0x266f8, True),
            (4, 0x9da4, 0x9c3c, True), (4, 0x9e00, 0x10258, True),
            (4, 0x9fd0, 0x9e74, True), (4, 0xa02c, 0x10270, True), (4, 0xa11c, 0x9f90, True)):
        offset = base + sections[index][4] + site - sections[index][3]
        word = _bootstrap_word(payload, offset)
        displacement = (word >> 7) & 0xfffff
        displacement -= (1 << 20) if displacement & (1 << 19) else 0
        if word & 0xf800007f != (0x28000020 if delay else 0x28000000) or site + 4 + 4 * displacement != target:
            raise FormatError("PPB saved-context original ARC call does not match")
        arc_calls.append({"section_index": index, "call_elf_virtual_address": site,
                          "original_target_elf_value": target, "normal_delay_slot": delay})
    if [_bootstrap_word(payload, 0x2e5c4 + 28 * i) for i in range(4)] != [0x3f940] * 4:
        raise FormatError("PPB saved-context H264 profile table does not match")
    return {"basis": {"model": "saved-arc-context-source-bridge-v1", "conditional": True,
                      "region_count": len(validated), "validated_bytes": total, "complete_arm_body_count": 20},
        "validated_regions": validated, "arc_bodies": bodies, "arm_calls": calls, "arc_calls": arc_calls,
        "critical_arm_words": [{"blob_file_offset": p, "instruction": word} for p, word in words],
        "roots": {"working_base": 0xd3a00, "working_stride": 0x1cc, "slot_count": 4,
                  "working_handle_offset": 0x20, "controller_root_word_address": 0xd3a08,
                  "controller_root_is_pointer_value": True, "handle_controller_offset": 0x64,
                  "handle_publication_store": 0xfbb0, "controller_publication_store": 0xecf0},
        "objects": {"declared_small_heap_bytes": dict(_PPB_CONTEXT_OBJECT_BYTES),
                    "allocation_call_sites": {"H": 0xf8d0, "C": 0xe8e0, "Q": 0x2b444, "M": 0x29110},
                    "payload_min": 0xd53dc, "end_exclusive": 0x116000,
                    "C_manager_pointer_offset": 0x1a0, "Q_controller_offset": 0,
                    "Q_map_pointer_offset": 8, "Q_video_bytes_offset": 0x10, "Q_video_virtual_offset": 0x14,
                    "C_video_tuple_offsets": [0x1d4, 0x1d8, 0x1dc], "M_video_tuple_offsets": [0x28, 0x30, 0x34],
                    "M_inclusive_virtual_bounds_offsets": [0x18, 0x1c], "M_route_word_offset": 0x40,
                    "stack_settings_byte5_store": 0x2b550, "child_M_route_word_under_conditions": 1,
                    "G_address": 0x116004, "G_bytes": 0x64, "G_identity_base": 0x116004,
                    "G_inclusive_virtual_bounds": [0x116068, 0x3ffc000]},
        "physical_context": {"H_physical_offset": 8, "H_submitted_bytes_offset": 0xc,
                    "H_imported_heap_offset": 0xcc, "default_requires_imported_heap_zero": True,
                    "H_selected_map_offset": 0x224, "selected_default_map_is_G": True,
                    "translation_equation": "P = G[0x30] + V - G[0x28]", "physical_slice_store": 0x25fbc,
                    "open_packet_offsets": [0x10, 0x14], "open_packet_stores": [0x274ec, 0x274f4],
                    "normalization_sites": [0x249f4, 0x24a00, 0x24a04, 0x24a10],
                    "normalization_equation": "D = P + ((-P) & 3); remaining = N - ((-P) & 3)",
                    "minimum_remaining_bytes_under_base_model": 0x177cc, "ordinary_h264_submitted_bytes": 0x3f940,
                    "profile_table_offsets": [0x2e5c4 + 28 * i for i in range(4)],
                    "length_output_pointer_site": 0xfa3c, "length_output_store": 0x25730},
        "saved_core": {"bytes": 0x5bc, "active_ARC_local_address": 0x3fffcdac,
                    "channel_table_D_store": 0x26798, "initial_core_clear_call": 0x26758,
                    "restore_call": 0x9fd0, "restore_destination_delay_slot": 0x9fd4,
                    "active_bit_set_sites": [0xa08c, 0xa090],
                    "save_call": 0x9da4, "save_source_delay_slot": 0x9da8,
                    "save_precedes_h264_save_call": 0x9e00, "active_bit_clear_sites": [0x9e60, 0x9e64],
                    "DMA_chunk_max_bytes": 128, "DMA_final_sync_site": 0x9d1c,
                    "fixed_scalar_spans": [{"role": role, "offset": off, "bytes": size} for role, off, size in _PPB_SAVED_SCALARS]},
        "validation_scope": {"original_section_qualified_bodies": True, "observer_guards_only": True,
                    "device_observed": False, "native_allocator_proven": False, "current_active_context_proven": False,
                    "source_lease_proven": False, "source_generation_proven": False, "source_extent_proven": False,
                    "runtime_relocation_validated": False, "ARC_local_DRAM_alias_proven": False},
        "required_conditions": ["Selected default/no-import path, valid unchanged typed objects and initialized maps.",
                    "Malloc/heap invariants and opaque callees preserved; full envelopes alone do not certify allocations.",
                    "Low ARM malloc namespace has no pointer rebase; native DRAM identity needs its address-domain premise."],
        "limitations": ["Saved D can be old while active; chunked DMA and host reads are non-atomic.",
                    "Stable passes do not prove a lease, generation, current context or source-plane extent.",
                    "Common-header geometry exponent/mask are outside saved D; no pool or plane value is followed."]}


def _ppb_stop_response(payload, busy):
    """Observed STOP words cannot certify the internal backend transaction."""
    if (type(payload) not in (bytes, bytearray) or len(payload) != 252 or
            MAX_PPB_STOP_RESPONSE_BYTES < 252 or type(busy) is not int or not 0 <= busy <= 255):
        raise FormatError("PPB STOP model requires exactly 252 bytes and a strict u8 busy byte")
    command, status = struct.unpack_from("<II", payload)
    request = struct.pack("<II", 0x73760006, 0) + bytes(244)
    return {"observed_words": [command, status], "busy_byte": busy,
            "stop_command_matches": command == 0x73760006, "zero_status_word": status == 0,
            "busy_zero": busy == 0, "channel_zero_request_equals_success_record": payload == request,
            "unacknowledged_request_collision": payload == request and busy == 0,
            "completion_certified": False, "backend_success_proven": False, "backend_failure_proven": False,
            "observed_inputs_only": True, "model_no_native_certification": True}


def _ppb_stop_context_bridge(payload):
    """Conditional STOP-to-save ordering, including the outer status mask."""
    total = sum(size for _, _, size, _ in _PPB_STOP_REGIONS)
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or len(_PPB_STOP_REGIONS) > MAX_PPB_STOP_REGIONS or
            total > MAX_PPB_STOP_BYTES or len(_PPB_STOP_RELOCATIONS) > MAX_PPB_STOP_RELOCATIONS):
        raise FormatError("PPB STOP context validation size/budget does not match")
    validated = []
    for role, offset, size, digest in _PPB_STOP_REGIONS:
        if hashlib.sha256(bounded(payload, offset, size, "PPB STOP context region")).hexdigest() != digest:
            raise FormatError(f"PPB STOP context region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": digest})
    saved = _ppb_saved_context_bridge(payload)
    base = 0x2ea60
    sections = [struct.unpack_from("<10I", payload, 0x79540 + i * 40) for i in range(55)]
    names = payload[0x67a95:0x69b6f]
    symbols = [struct.unpack_from("<IIIBBH", payload, p) for p in range(0x69b70, 0x6cfc0, 16)]
    def name(symbol):
        offset = symbol[0]
        if offset >= len(names) or names.find(b"\0", offset) < 0:
            raise FormatError("PPB STOP original symbol name does not match")
        return names[offset:names.index(b"\0", offset)].decode("ascii")
    bodies = list(saved["arc_bodies"])
    for label, index, start, end, offset, digest in _PPB_STOP_ARC_BODIES:
        section = sections[index]
        matches = [i for i, s in enumerate(symbols) if name(s) == label and
                   s[1:3] == (start, end - start) and s[3] & 15 == 2 and s[5] == index]
        if (len(matches) != 1 or not section[3] <= start < end <= section[3] + section[5] or
                base + section[4] + start - section[3] != offset):
            raise FormatError(f"PPB STOP section-qualified body {label} does not match")
        bodies.append({"name": label, "section_index": index, "symbol_index": matches[0],
                       "elf_virtual_address": start, "size": end - start, "blob_file_offset": offset, "sha256": digest})
    relocations = []
    for record, owner, site, symbol_index, label, target_section, target, delay, _ in _PPB_STOP_RELOCATIONS:
        table = sections[39 if owner == 4 else 51]
        location, info, addend = struct.unpack_from("<IIi", payload, record)
        symbol = symbols[symbol_index]
        source = sections[owner]
        offset = base + source[4] + site - source[3]
        word = _bootstrap_word(payload, offset)
        displacement = (word >> 7) & 0xfffff
        displacement -= (1 << 20) if displacement & (1 << 19) else 0
        if ((table[1], table[6], table[7], table[9]) != (4, 35, owner, 12) or
                not base + table[4] <= record <= base + table[4] + table[5] - 12 or
                (record - base - table[4]) % 12 or (location, info, addend) != (site, symbol_index << 8 | 6, 0) or
                (name(symbol), symbol[5], symbol[1], symbol[3] & 15) != (label, target_section, target, 2) or
                not any(b["section_index"] == owner and b["elf_virtual_address"] <= site <=
                        b["elf_virtual_address"] + b["size"] - 4 for b in bodies) or
                not sections[target_section][3] <= target < sections[target_section][3] + sections[target_section][5] or
                word & 0xf800007f != (0x28000020 if delay else 0x28000000) or site + 4 + 4 * displacement != target):
            raise FormatError("PPB STOP selected numeric RELA/direct call does not match")
        relocations.append({"record_blob_file_offset": record, "source_section_index": owner,
            "call_elf_virtual_address": site, "type": 6, "addend": 0, "symbol_index": symbol_index,
            "symbol": label, "target_section_index": target_section, "original_target_elf_value": target,
            "normal_delay_slot": delay, "runtime_application_validated": False})
    words = ((0x4408, 0xe5c590d2), (0xef98, 0xe5d40004), (0xefac, 0xe3a00000), (0xefb8, 0xeaffffe1),
        (0x27798, 0xe5890000), (0x2779c, 0xe5897004), (0x277bc, 0xe1a08000), (0x277d4, 0xe1a00008),
        (0x270f4, 0xe5c4008c), (0x27170, 0xe5c4008c), (0x271b4, 0xe5c4008c), (0x27ab0, 0x73760006))
    if any(_bootstrap_word(payload, p) != word for p, word in words):
        raise FormatError("PPB STOP critical A32 word does not match")
    calls = []
    for site, target in ((0x4410, 0x125c), (0x128c, 0xef10), (0xef54, 0x24edc), (0xef84, 0xe548),
            (0xef94, 0x27750), (0x277b8, 0x2705c), (0xf778, 0x26158), (0xf7d8, 0x2056c)):
        word = _bootstrap_word(payload, site)
        displacement = word & 0xffffff
        displacement -= (1 << 24) if displacement & (1 << 23) else 0
        if word & 0xff000000 != 0xeb000000 or site + 8 + 4 * displacement != target:
            raise FormatError("PPB STOP original A32 BL does not match")
        calls.append({"call_blob_file_offset": site, "target_blob_file_offset": target})
    return {"basis": {"model": "post-host-stop-saved-context-v1", "conditional": True,
                "region_count": len(validated), "validated_bytes": total},
        "validated_regions": validated, "arc_bodies": bodies, "arm_calls": calls, "reused_arc_calls": saved["arc_calls"],
        "critical_arm_words": [{"blob_file_offset": p, "instruction": word} for p, word in words],
        "original_numeric_relocations": relocations,
        "conditional_arc_success_order": [0x25910, 0x26db4, 0x9da4, 0x9d1c, 0x259cc, 0x259d4, 0x259d8, 0x259ec],
        "status_mask": {"internal_stop_command": 0x73760006, "builder_transport_call": 0x277b8,
                "discarded_builder_return_call": 0xef94, "overwriting_load": 0xef98, "outer_zero_return_site": 0xefac,
                "request_channel_word_offset": 4, "reply_status_word_offset": 4, "busy_clear_is_success_only": False,
                "channel_zero_counterexample": _ppb_stop_response(struct.pack("<II", 0x73760006, 0) + bytes(244), 0)},
        "direct_stop_graph_lifetime": {"working_active_byte_offset": 0xc4, "working_started_byte_offset": 0xd2,
                "started_clear_store": 0x4408, "handle_publication_offset": 0x20,
                "selected_direct_stop_unpublishes_graph": False, "close_context_release_call": 0xf778,
                "close_handle_free_call": 0xf7d8, "opaque_callee_preservation_required": True,
                "native_allocation_lifetime_certified": False},
        "candidate": {"stage": "host-STOP-returned/pre-CLOSE", "conditional_arc_acknowledged": False,
                "same_handle_and_frozen_H_C_Q_M_P_N_and_map_tuples_required": True,
                "slot0_active_low_byte": 1, "slot0_started_byte": 0, "other_slots_active_low_byte": 0,
                "graph_calls_each": 31, "graph_bytes_each": 156, "saved_scalar_calls_each": 8, "saved_scalar_bytes_each": 240,
                "passes_per_stage": 2, "stage_count": 4, "per_pass_calls": 70, "per_pass_bytes": 552,
                "total_calls": 560, "total_bytes": 4416, "saved_scalar_spans": saved["saved_core"]["fixed_scalar_spans"],
                "command_buffer_or_plane_or_guessed_MMIO_reads": False, "observed_stability_is_atomic": False},
        "validation_scope": {"original_section_qualified_bodies": True, "selected_numeric_RELA": True,
                "device_observed": False, "backend_stop_completion_proven": False, "guaranteed_saved_refresh": False,
                "current_live_descriptors_proven": False, "source_lease_proven": False, "source_generation_proven": False},
        "limitations": ["Outer ARM STOP success can mask internal timeout, echo or backend failure.",
                "Channel-zero unacknowledged STOP request and zero-success reply can be byte-identical, even with busy zero.",
                "ARC-success ordering assumes coherent DMA and original call edges; numerical RELA receipts are not runtime certification.",
                "STOP changes references/descriptors, so a later D difference is not pure pre-STOP staleness evidence.",
                "Direct source lifetime excludes opaque aliasing/mutation; envelopes and stable passes certify no live allocation or lease."]}


def _ppb_fixed_metadata_window(physical, submitted_bytes, video_base, video_bytes):
    """Fixed original-layout scalar targets, not metadata-derived addresses."""
    window = _ppb_saved_context_window(physical, submitted_bytes, video_base, video_bytes)
    context = window["context_address"]
    envelopes = [{"role": role, "offset": offset, "address": context + offset, "bytes": size}
                 for role, offset, size in (("delivery_ring", 0x15678, 256),
                                           ("return_ring", 0x15778, 256), ("metadata_pool", 0x15878, 34 * 228))]
    if any(item["address"] + item["bytes"] > window["context_end_exclusive"] for item in envelopes):
        raise FormatError("PPB complete fixed metadata envelope exceeds the declared slice")
    spans = [{"role": role, "offset": offset, "address": context + offset, "bytes": 8}
             for role, offset in (("delivery_indices", 0x15678), ("return_indices", 0x15778))]
    spans += [{"role": "metadata_prefix", "slot": slot, "offset": 0x15878 + 228 * slot,
               "address": context + 0x15878 + 228 * slot, "bytes": 72} for slot in range(34)]
    return {"context_address": context, "context_bytes": window["context_bytes"],
            "context_end_exclusive": window["context_end_exclusive"], "alignment_skip": window["alignment_skip"],
            "whole_envelopes": envelopes, "read_spans": spans, "read_calls": 36, "total_read_bytes": 2464,
            "max_read_bytes": 72, "no_observed_pointer_following": True, "source_plane_access": False,
            "observed_inputs_only": True, "model_no_native_certification": True, "non_atomic": True,
            "current_state_certified": False, "source_lease_certified": False, "generation_certified": False}


def _ppb_fixed_metadata_bridge(payload):
    """Conditional publication/return layout outside the periodically saved core."""
    total = sum(size for _, _, size, _ in _PPB_FIXED_METADATA_REGIONS)
    if (type(payload) not in (bytes, bytearray) or len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            len(_PPB_FIXED_METADATA_REGIONS) > MAX_PPB_FIXED_METADATA_REGIONS or total > MAX_PPB_FIXED_METADATA_BYTES or
            len(_PPB_FIXED_METADATA_CALLS) > MAX_PPB_FIXED_METADATA_CALLS):
        raise FormatError("PPB fixed metadata size/type/budget does not match")
    regions = []
    for role, offset, size, digest in _PPB_FIXED_METADATA_REGIONS:
        if hashlib.sha256(bounded(payload, offset, size, "PPB fixed metadata region")).hexdigest() != digest:
            raise FormatError(f"PPB fixed metadata region {role} does not match")
        regions.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": digest})
    saved = _ppb_saved_context_bridge(payload)
    handoff = _arm_ppb_metadata_handoff(payload)
    mappings, dependencies, relocation_count = _ppb_bank_elf_context(payload)
    sections = [struct.unpack_from("<10I", payload, 0x79540 + index * 40) for index in range(55)]
    names = payload[0x67a95:0x69b6f]
    symbols = [struct.unpack_from("<IIIBBH", payload, cursor) for cursor in range(0x69b70, 0x6cfc0, 16)]
    bodies = []
    for name, section_index, start, end, offset, digest in _PPB_FIXED_METADATA_BODIES:
        section = sections[section_index]
        matches = [index for index, symbol in enumerate(symbols) if symbol[0] < len(names) and
                   names[symbol[0]:].split(b"\0", 1)[0] == name.encode("ascii") and
                   symbol[1:3] == (start, end - start) and symbol[3] & 15 == 2 and symbol[5] == section_index]
        if (len(matches) != 1 or not section[3] <= start < end <= section[3] + section[5] or
                0x2ea60 + section[4] + start - section[3] != offset):
            raise FormatError(f"PPB fixed metadata qualified body {name} does not match")
        bodies.append({"name": name, "section_index": section_index, "symbol_index": matches[0],
                       "elf_virtual_address": start, "size": end - start, "blob_file_offset": offset, "sha256": digest})
    words = ((16, 0x26748, 0x42807c00), (16, 0x2674c, 0x5bc), (16, 0x2676c, 0x41a02800),
        (16, 0x267b0, 0x10009b30), (16, 0x268d8, 0x679ffe21), (16, 0x268dc, 0x10060000),
        (16, 0x268e0, 0x40007c00), (16, 0x268e4, 0x150e4), (16, 0x268e8, 0x1001013c),
        (16, 0x268ec, 0x10000644), (16, 0x26910, 0x10000644), (16, 0x26924, 0x40407c00),
        (16, 0x26928, 0x14ee4), (16, 0x2692c, 0x10008524), (16, 0x26938, 0x40407c00),
        (16, 0x2693c, 0x14fe4), (16, 0x26958, 0x10008528), (16, 0x3b308, 0x401ffeec),
        (4, 0xb8fc, 0x2ff34fa0), (4, 0xb900, 0x605ffee4), (4, 0xb904, 0x2ff34b80),
        (4, 0xb980, 0x2fff7a20), (4, 0xb984, 0x40292800), (4, 0xb988, 0x28008c00),
        (4, 0xb99c, 0x68007c00), (4, 0xb9a0, 0x1000), (4, 0xb9a4, 0x10808190))
    critical = []
    for section_index, address, word in words:
        offset = 0x2ea60 + sections[section_index][4] + address - sections[section_index][3]
        if _bootstrap_word(payload, offset) != word:
            raise FormatError("PPB fixed metadata critical original word does not match")
        critical.append({"section_index": section_index, "elf_virtual_address": address,
                         "blob_file_offset": offset, "instruction_or_literal": word})
    calls = []
    for record, source_section, site, index, name, target_section, target, delay in _PPB_FIXED_METADATA_CALLS:
        matches = [item for item in dependencies if item["relocation_record_blob_file_offset"] == record and
                   item["elf_virtual_address"] == site and item["type"] == 6 and item["symbol_index"] == index and
                   item["symbol"] == name and item["addend"] == 0 and item["target_section_index"] == target_section and
                   item["original_target_elf_value"] == target and
                   item["blob_file_offset"] == 0x2ea60 + sections[source_section][4] + site - sections[source_section][3]]
        if len(matches) != 1:
            raise FormatError("PPB fixed metadata numeric relocation edge does not match")
        word = _bootstrap_word(payload, matches[0]["blob_file_offset"])
        displacement = (word >> 7) & 0xfffff
        displacement -= (1 << 20) if displacement & (1 << 19) else 0
        if word & 0xf800007f != (0x28000020 if delay else 0x28000000) or site + 4 + 4 * displacement != target:
            raise FormatError("PPB fixed metadata original ARC call does not match")
        calls.append(dict(matches[0], source_section_index=source_section, instruction=word, normal_delay_slot=delay))
    return {"basis": {"model": "fixed-dram-metadata-publication-v1", "conditional": True,
                      "region_count": len(regions), "validated_bytes": total, "complete_relocation_records": relocation_count},
        "validated_regions": regions, "arc_bodies": bodies, "section_mappings": mappings,
        "critical_words": critical, "relocations": calls, "arm_handoff": handoff,
        "layout": {"normalized_D_equation": "P + ((-P) & 3)", "saved_core_bytes": 0x5bc,
                   "driver_context_bytes": 236 << 1, "frame_records_offset": 0x794,
                   "frame_record_bytes": 284, "frame_record_count": 63, "frame_records_end_exclusive": 0x4d78,
                   "delivery_ring_offset": 0x15678, "return_ring_offset": 0x15778, "ring_bytes": 256,
                   "metadata_pool_offset": 0x15878, "metadata_record_bytes": 228, "metadata_record_count": 34,
                   "metadata_pool_end_exclusive": 0x176c0, "minimum_remaining_bytes": 0x177cc,
                   "constructor_clears_each_metadata_word_offset": 0x44, "constructor_clears_whole_pool": False},
        "observation": {"flag": "--observe-ppb-metadata", "stages": 4, "passes_per_stage": 2,
                        "fixed_calls_per_pass": 36, "fixed_bytes_per_pass": 2464,
                        "graph_calls_per_pass": 62, "graph_bytes_per_pass": 312,
                        "trial_calls": 784, "trial_bytes": 22208, "max_read_bytes": 72,
                        "pool_and_plane_pointer_following": False, "target_writes": False, "non_atomic": True},
        "publication": {"whole_metadata_DMA_write_site": 0xb8fc, "whole_metadata_DMA_sync_site": 0xb904,
                        "frame_ancillary_clear_DMA_site": 0xb938, "frame_ancillary_clear_sync_site": 0xb940,
                        "delivery_queue_put_site": 0xb980, "delivery_notify_site": 0xb988,
                        "display_descriptor_bit_set_site": 0xb9a4, "display_descriptor_bit": 0x1000,
                        "display_bit_set_after_notify": True, "queued_return_is_consumption": False},
        "validation_scope": {"original_section_qualified_bodies": True, "observer_guards_only": True,
                             "device_observed": False, "runtime_relocation_validated": False,
                             "current_live_state": False, "allocator_integrity": False, "active_frame_extent": False,
                             "source_lease": False, "generation": False, "all_consumer_completion": False,
                             "backend_stop_completion": False, "standalone_processing": False},
        "conditions": saved["required_conditions"],
        "limitations": ["Fixed layout assumes successful original constructor and preservation by opaque callees.",
                        "Metadata prefixes are separately DMA-published, not the saved core; old or partially updated fields can survive.",
                        "Presentation marker +3c, ancillary words +40/+44, recycled addresses and ring indices are not generation tokens.",
                        "Picture metadata flags are not the active ARC descriptor bitmap; neither proves a raw-source lease.",
                        "Host STOP success is not ARC completion; queued returns and matching sequential reads do not establish consumption."]}


def _ppb_return_header_window(handle, physical, submitted_bytes, video_base, video_bytes):
    """Model bounded handle scalars and fixed headers; never follow route values."""
    owner = _ppb_context_object_window(handle, "H")
    fixed = _ppb_fixed_metadata_window(physical, submitted_bytes, video_base, video_bytes)
    context = fixed["context_address"]
    spans = [fixed["read_spans"][0]]
    spans += [{"role": role, "offset": offset, "address": context + offset, "bytes": size}
              for role, offset, size in (("return_initial", 0x15778, 8),
                  ("return_write_first", 0x1577c, 4), ("return_write_second", 0x1577c, 4),
                  ("return_final", 0x15778, 8))]
    spans += fixed["read_spans"][2:]
    fixed.update(read_spans=spans, read_calls=39, total_read_bytes=2480)
    return {"handle_window": owner,
            "handle_route_span": {"role": "handle_route_words", "offset": 0x250,
                                  "address": handle + 0x250, "bytes": 8},
            "fixed_window": fixed, "read_calls": 40, "total_read_bytes": 2488,
            "max_read_bytes": 72, "no_observed_pointer_following": True,
            "observed_inputs_only": True, "model_no_native_certification": True, "non_atomic": True}


def _ppb_return_header_observation(route_bytes, return_bytes):
    """Detach six sequential samples without interpreting their routing or cause."""
    if (type(route_bytes) not in (bytes, bytearray) or len(route_bytes) != 8 or
            type(return_bytes) not in (bytes, bytearray) or len(return_bytes) != 24):
        raise FormatError("PPB return observation requires exactly 8 route bytes and 24 sample bytes")
    routes, samples = list(struct.unpack("<2I", route_bytes)), list(struct.unpack("<6I", return_bytes))
    return {"route_words": routes, "return_samples": samples,
            "return_sample_roles": ["initial_read", "initial_write", "first_single_write",
                                    "second_single_write", "final_read", "final_write"],
            "route_zero_indices": [index for index, word in enumerate(routes) if word == 0],
            "route_all_ones_indices": [index for index, word in enumerate(routes) if word == 0xffffffff],
            "routes_differ": routes[0] != routes[1],
            "return_zero_indices": [index for index, word in enumerate(samples) if word == 0],
            "return_all_ones_indices": [index for index, word in enumerate(samples) if word == 0xffffffff],
            "return_read_samples_equal": samples[0] == samples[4],
            "return_write_samples_equal": len({samples[index] for index in (1, 2, 3, 5)}) == 1,
            "observed_inputs_only": True, "model_no_native_certification": True, "non_atomic": True,
            "certified": dict.fromkeys(("routing_identity", "metadata_validity", "current_state", "source_lease",
                "generation", "backend_completion", "atomic_64bit_read", "request_width_cause", "zero_cause"), False)}


def _ppb_return_header_bridge(payload):
    """Reuse the stock fixed-pool proof for a separately opted-in scalar schedule."""
    fixed = _ppb_fixed_metadata_bridge(payload)
    words = (("acquire", 0xd638, 0xe5976250, 0x250), ("release", 0xd5b0, 0xe5986254, 0x254))
    critical = []
    for role, address, word, offset in words:
        if _bootstrap_word(payload, address) != word:
            raise FormatError("PPB return header handle-route load does not match")
        critical.append({"role": role, "blob_file_offset": address, "instruction": word,
                         "handle_word_offset": offset})
    return {"basis": dict(fixed["basis"], model="fixed-return-header-observation-v1"),
        "validated_regions": fixed["validated_regions"], "critical_arm_words": critical,
        "reused_fixed_metadata_source": {"arc_bodies": fixed["arc_bodies"], "relocations": fixed["relocations"],
                                       "publication": fixed["publication"], "layout": fixed["layout"]},
        "handle_route": {"kind": "H", "whole_object_bytes": 0xa84c, "offset": 0x250, "bytes": 8,
                         "word_offsets": [0x250, 0x254], "word_roles": ["acquire_route_word", "return_route_word"],
                         "full_small_heap_guard_required": True, "frozen_owner_graph_required": True,
                         "route_contents_followed": False, "runtime_routing_identity_proven": False},
        "return_reads": [{"role": role, "context_offset": offset, "bytes": size}
                         for role, offset, size in (("return_initial", 0x15778, 8),
                             ("return_write_first", 0x1577c, 4), ("return_write_second", 0x1577c, 4),
                             ("return_final", 0x15778, 8))],
        "observation": {"flag": "--observe-ppb-return", "stages": 4, "passes_per_stage": 2,
            "ordered_pass": ["graph_before", "handle_route_words", "delivery_indices", "return_initial",
                "return_write_first", "return_write_second", "return_final", "metadata_prefixes", "graph_after"],
            "graph_calls_each": 31, "graph_bytes_each": 156, "handle_calls_per_pass": 1, "handle_bytes_per_pass": 8,
            "fixed_calls_per_pass": 39, "fixed_bytes_per_pass": 2480, "per_pass_calls": 102, "per_pass_bytes": 2800,
            "trial_calls": 816, "trial_bytes": 22400, "max_read_bytes": 72,
            "pool_and_plane_pointer_following": False, "diagnostic_target_writes": False, "non_atomic": True},
        "transport": {"api_length_unit": "bytes", "driver_length_unit": "DWORDs", "pair_api_bytes": 8,
            "pair_driver_dwords": 2, "pair_sequential_readl_calls": 2, "readl_address_step_bytes": 4,
            "host_window_locked_per_burst": True, "firmware_words_locked": False,
            "atomic_64bit_read": False, "successful_api_certifies_firmware_value": False,
            "request_width_cause_certified": False, "runtime_source_validation": False},
        "validation_scope": dict(fixed["validation_scope"], runtime_routing_identity=False,
            metadata_validity=False, atomic_64bit_read=False, request_width_cause=False, zero_cause=False),
        "conditions": fixed["conditions"],
        "limitations": fixed["limitations"] + [
            "H+0x250/H+0x254 are observed routing scalars only; zero, all-ones or differing values do not establish pointer identity or validity.",
            "An 8-byte API request performs two sequential driver readl operations, not an atomic 64-bit firmware snapshot.",
            "Preserve all six return DWORD samples separately; equal or differing samples do not attribute a zero index or request-width cause."]}


def _ppb_source_geometry_join(metadata_mb, prefix_mb, metadata_config, allocation_config):
    """Compare supplied snapshots, not native frame identity or ownership."""
    for dimensions in (metadata_mb, prefix_mb):
        if (not isinstance(dimensions, (tuple, list)) or len(dimensions) != 2 or
                any(type(value) is not int or not 0 <= value <= 255 for value in dimensions)):
            raise FormatError("PPB source dimensions require two u8 macroblock values")
    for config in (metadata_config, allocation_config):
        if (not isinstance(config, (tuple, list)) or len(config) != 3 or
                type(config[0]) is not int or not 0 <= config[0] < 32 or
                type(config[1]) is not int or not 0 <= config[1] <= 255 or type(config[2]) is not bool):
            raise FormatError("PPB source config requires stripe shift, byte mask and boolean extra")
    metadata = _ppb_bank_geometry(*(value * 16 for value in metadata_mb), *metadata_config)
    allocation = _ppb_bank_geometry(*(value * 16 for value in prefix_mb), *allocation_config)
    return {"metadata_mb_dimensions": list(metadata_mb), "prefix_mb_dimensions": list(prefix_mb),
            "metadata_config": list(metadata_config), "allocation_config": list(allocation_config),
            "metadata_geometry": metadata, "allocation_geometry": allocation,
            "dimensions_equal": list(metadata_mb) == list(prefix_mb),
            "config_equal": list(metadata_config) == list(allocation_config),
            "conditional_geometry_equal": metadata == allocation,
            "observed_inputs_only": True, "model_no_native_certification": True}


def _ppb_source_frame_record(pool, index):
    """Bound a supplied frame-record equation; these are model-only guards."""
    pool = _ppb_bank_u32(pool, "source pool")
    if type(index) is not int or not 0 <= index <= 62:
        raise FormatError("PPB source model frame index must be 0..62")
    record = pool + 284 * index
    if record + 283 > 0xffffffff:
        raise FormatError("PPB source model complete frame record overflows u32")
    return {"pool": pool, "index": index, "stride_bytes": 284, "record_address": record,
            "prefix_address": record, "prefix_bytes": 56, "metadata_address": record + 56,
            "metadata_bytes": 228, "metadata_source_words_address": record + 60,
            "metadata_source_words_bytes": 8, "metadata_chroma_offset_address": record + 64,
            "metadata_chroma_offset_bytes": 4, "record_last_byte": record + 283,
            "overflow_guard_is_model_only": True, "firmware_index_or_overflow_guard_proven": False,
            "model_no_native_certification": True}


def _ppb_source_provenance(payload):
    """Pinned ordinary H264 source equations; never native lease certification."""
    total = sum(size for _, _, size, _ in _PPB_SOURCE_REGIONS)
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or
            len(_PPB_SOURCE_REGIONS) > MAX_PPB_SOURCE_REGIONS or total > MAX_PPB_SOURCE_BYTES or
            MAX_PPB_SOURCE_RELOCATIONS < 2685):
        raise FormatError("PPB source validation size/budget does not match")
    validated = []
    for role, offset, size, digest in _PPB_SOURCE_REGIONS:
        if hashlib.sha256(bounded(payload, offset, size, "PPB source region")).hexdigest() != digest:
            raise FormatError(f"PPB source region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": digest})
    bank = _ppb_bank_contract(payload)
    # Independently resolve numeric symbol indexes in the original section map.
    base = 0x2ea60
    sections = [struct.unpack_from("<10I", payload, 0x79540 + i * 40) for i in range(55)]
    section_names, symbol_names = payload[0x79098:0x7953f], payload[0x67a95:0x69b6f]

    def string(table, offset):
        if offset >= len(table) or table.find(b"\0", offset) < 0:
            raise FormatError("PPB source original string offset does not match")
        return table[offset:table.index(b"\0", offset)].decode("ascii")

    mappings = list(bank["containing_sections"])
    for index, name, address, offset, size in (
            (3, ".h264_critical_code_slice", 0x63d4, 0x2818, 0x1bb8),
            (5, ".h264_critical_code_picture", 0xc158, 0x859c, 0x42b0)):
        section = sections[index]
        if (string(section_names, section[0]) != name or
                (section[1], section[3], section[4], section[5]) != (1, address, offset, size)):
            raise FormatError("PPB source containing section does not match")
        mappings.append({"section_index": index, "name": name, "elf_virtual_address": address,
                         "elf_file_offset": offset, "blob_file_offset": base + offset, "size": size,
                         "blob_minus_elf_address": base + offset - address})
    symbols = [struct.unpack_from("<IIIBBH", payload, offset)
               for offset in range(0x69b70, 0x69b70 + 13392, 16)]
    bodies = []
    for name, index, start, end, offset, digest in _PPB_SOURCE_BODIES:
        section = sections[index]
        matches = [i for i, s in enumerate(symbols) if string(symbol_names, s[0]) == name and
                   s[1:3] == (start, end - start) and s[3] & 15 == 2 and s[5] == index]
        if (len(matches) != 1 or not section[3] <= start < end <= section[3] + section[5] or
                base + section[4] + start - section[3] != offset):
            raise FormatError(f"PPB source section-qualified body {name} does not match")
        bodies.append({"name": name, "section_index": index, "symbol_index": matches[0],
                       "elf_virtual_address": start, "size": end - start,
                       "blob_file_offset": offset, "sha256": digest})
    expected = {0x6f3cc: (5, 0xf730, 585, "Core_PopulatePPB", 4, 0x9344),
                0x6f69c: (5, 0x100ec, 671, "H264_ParseSPS", 16, 0x2aee4),
                0x6d80c: (3, 0x7840, 659, "H264_StartOfPicture", 5, 0xd8bc),
                0x6d458: (3, 0x64b0, 672, "H264_ActivateSPS", 16, 0x2b658),
                0x74730: (16, 0x2b5ac, 790, "memcpy", 2, 0x5550),
                0x7479c: (16, 0x2b6f4, 641, "Core_LocalCopy", 2, 0x52e0)}
    dependencies, count = [], 0
    for index, owner, offset, size in ((38, 3, 0x6d410, 1572), (40, 5, 0x6e670, 4596),
                                       (51, 16, 0x72780, 26052)):
        if (sections[index][1], base + sections[index][4], sections[index][5], sections[index][6],
                sections[index][7], sections[index][9]) != (4, offset, size, 35, owner, 12):
            raise FormatError("PPB source original relocation section does not match")
        for cursor in range(offset, offset + size, 12):
            count += 1
            location, info, addend = struct.unpack_from("<IIi", payload, cursor)
            symbol_index, kind = info >> 8, info & 255
            if count > MAX_PPB_SOURCE_RELOCATIONS or symbol_index >= len(symbols) or kind not in (0, 4, 6, 7):
                raise FormatError("PPB source relocation budget/kind/index does not match")
            if cursor not in expected:
                continue
            symbol = symbols[symbol_index]
            name = string(symbol_names, symbol[0])
            if ((owner, location, symbol_index, name, symbol[5], symbol[1]) != expected[cursor] or
                    kind != 6 or addend != 0 or symbol[3] & 15 != 2 or
                    not sections[owner][3] <= location <= sections[owner][3] + sections[owner][5] - 4):
                raise FormatError("PPB source selected numeric relocation does not match")
            target = sections[symbol[5]]
            if not target[3] <= symbol[1] < target[3] + target[5]:
                raise FormatError("PPB source original relocation target is outside its section")
            dependencies.append({"source_section_index": owner, "elf_virtual_address": location,
                                 "relocation_record_blob_file_offset": cursor, "type": kind,
                                 "symbol_index": symbol_index, "symbol": name, "addend": addend,
                                 "target_section_index": symbol[5], "original_target_elf_value": symbol[1],
                                 "original_target_blob_file_offset": base + target[4] + symbol[1] - target[3],
                                 "runtime_application_validated": False})
    if count != 2685 or len(dependencies) != len(expected):
        raise FormatError("PPB source complete relocation receipt does not match")
    calls = []
    for owner, index, site, callee, target, delay in (
            ("ParseSlice", 3, 0x7840, "H264_StartOfPicture", 0xd8bc, True),
            ("PopulatePPB", 5, 0xf730, "Core_PopulatePPB", 0x9344, True),
            ("EndOfPicture", 5, 0xf998, "PopulatePPB", 0xf2b0, False),
            ("H264_DecodePicture", 5, 0x100ec, "H264_ParseSPS", 0x2aee4, False),
            ("H264_DecodePicture", 5, 0x10164, "EndOfPicture", 0xf760, False),
            ("Core_PopulatePPB", 4, 0x94f4, "VideoParameters", 0x80dc, True),
            ("H264_ParseSPS", 16, 0x2b5ac, "memcpy", 0x5550, True),
            ("H264_ActivateSPS", 16, 0x2b6f4, "Core_LocalCopy", 0x52e0, True)):
        if not any(name == owner and sec == index and start <= site < end
                   for name, sec, start, end, _, _ in _PPB_SOURCE_BODIES):
            raise FormatError("PPB source call is outside its section-qualified body")
        offset = base + sections[index][4] + site - sections[index][3]
        word = struct.unpack_from("<I", payload, offset)[0]
        displacement = (word >> 7) & 0xfffff
        displacement -= (1 << 20) if displacement & (1 << 19) else 0
        if word & 0xf800007f != (0x28000020 if delay else 0x28000000) or site + 4 + 4 * displacement != target:
            raise FormatError("PPB source original direct call does not match")
        calls.append({"owner": owner, "section_index": index, "call_elf_virtual_address": site,
                      "call_blob_file_offset": offset, "callee": callee, "direct_target_elf_virtual_address": target,
                      "normal_delay_slot": delay})
    return {
        "basis": {"model": "ordinary-h264-ppb-source-v1", "conditional": True,
                  "outer_blob_file_offset": base, "complete_body_count": len(bodies), "code_bytes": 8668,
                  "reused_bank_complete_body_count": 35, "region_count": len(validated), "validated_bytes": total,
                  "address_spaces": "Original ELF, blob-file, ARC-local and host DRAM addresses remain distinct."},
        "validated_regions": validated, "containing_sections": mappings, "producer_bodies": bodies,
        "original_relocations": {"complete_table_record_count": count, "selected_records": dependencies,
                                 "numeric_original_records_not_runtime_relocation_proof": True}, "calls": calls,
        "ordinary_path": {"core_flag_address": 0x3fffcdac, "required_core_flag_mask": 1,
                          "start_guard_elf_virtual_addresses": [0xd8ec, 0xd8f4, 0xd8fc],
                          "end_guard_elf_virtual_addresses": [0x10158, 0x1015c, 0x10160],
                          "current_flags_address": 0x3fffc8b8, "flags_zero_store": 0xff28,
                          "excluded_current_flags_mask": 0x10, "ordinary_populate_call": 0xf998,
                          "ordinary_branch_guard_elf_virtual_addresses": [0xf988, 0xf98c],
                          "cfp2_populate_call": 0xf990, "cfp2_requires_current_and_prior_bit0": True,
                          "cfp2_guard_elf_range": [0xdba0, 0xdc04],
                          "cfp2_flag_or": 0xdc04, "cfp2_delay_slot_flag_store": 0xdc0c,
                          "cfp2_delay_slot_store_before_callee": True,
                          "empty_picture_path_excluded": True},
        "dimensions": {"active_sps_base": 0x3fffc430, "width_mb_address": 0x3fffc469,
                       "normalized_height_mb_address": 0x3fffc454, "pixel_dimension_shift": 4,
                       "metadata_mb_reads": [0xf300, 0xf30c], "metadata_pixel_stores": [0xf310, 0xf324],
                       "prefix_mb_reads": [0x10188, 0x10194], "prefix_pixel_stores": [0x10190, 0x101a0],
                       "prefix_height_normal_delay_slot": True,
                       "metadata_geometry_call": 0x94f4, "height_load_in_normal_delay_slot": 0x94f8,
                       "metadata_chroma_offset_store": 0x9518,
                       "metadata_chroma_offset_is_pre_display_relative": True,
                       "same_dimensions_preserved_required": True,
                       "same_geometry_config_preserved_required": True,
                       "conditional_join_requires": ["dimensions_equal", "config_equal", "conditional_geometry_equal"]},
        "publication": {"metadata_staging_address": 0x3fffd004, "metadata_bytes": 228,
                        "metadata_clear_call": 0xf2f8, "metadata_flags_delay_slot_store": 0xf734,
                        "flags_store_before_core_populate": True, "frame_record_bytes": 284,
                        "frame_prefix_bytes": 56, "prefix_local_copy_call": 0xa784,
                        "prefix_dma_write_call": 0xa7a8, "metadata_local_copy_call": 0xa7c4,
                        "metadata_dma_write_call": 0xa7d4, "dma_sync_call": 0xa800,
                        "producer_pool_pointer_address": 0x3fffcfc8, "reader_pool_pointer_address": 0x3fffd2dc,
                        "metadata_pool_pointer_address": 0x3fffd0e8,
                        "runtime_pool_identity_validated": False},
        "conditions": {"sps_copy_bytes": 296, "sps_parse_copy_call": 0x2b5ac,
                       "sps_activate_dma_read_call": 0x2b6dc, "sps_activate_dma_sync_call": 0x2b6e4,
                       "sps_activate_local_copy_call": 0x2b6f4, "sps_scratch_address": 0x30051c80,
                       "activation": bank["context_initialization"]["activation"],
                       "geometry_config": bank["context_initialization"]["init_geometry"],
                       "preservation_required": ["Same initialized channel, active SPS and geometry config across both reads.",
                                                 "Scratch, DMA and opaque local-copy/clear callees complete coherently.",
                                                 "Disjoint valid frame/metadata/context spans; no concurrent unmodeled mutation."]},
        "validation_scope": {"original_section_qualified_bodies": True, "original_numeric_relocations": True,
                             "observed_inputs_only": True, "model_no_native_certification": True,
                             "runtime_relocation": False, "vendor_ISA": False, "device_observed": False,
                             "native_frame_identity": False, "source_plane_extent": False,
                             "source_plane_lease": False, "generation_safe_reuse": False,
                             "cache_or_dma_completion": False, "all_consumers": False}}


def _ppb_bank_state(bank_bases, bank_bytes, descriptor_limit=34, stripe_exponent=5,
                    alignment_mask=63, metadata_extra=False):
    """Bounded original constructor projection; not a device buffer allocator."""
    if not isinstance(bank_bases, (tuple, list)) or len(bank_bases) > 9:
        raise FormatError("PPB bank model bank count must be 0..9")
    _ppb_bank_u32(bank_bytes, "bank bytes")
    if type(descriptor_limit) is not int or not 0 <= descriptor_limit <= 34:
        raise FormatError("PPB bank model descriptor limit must be 0..34")
    if (type(stripe_exponent) is not int or not 0 <= stripe_exponent <= 255 or
            type(alignment_mask) is not int or not 0 <= alignment_mask <= 255 or
            type(metadata_extra) is not bool):
        raise FormatError("PPB bank initialization parameters do not fit their stored fields")
    return {"flags": [0] * 34,
            "banks": [{"base": _ppb_bank_u32(base, "bank base"), "mask": 0, "stride": 0, "geometry": 0}
                      for base in bank_bases],
            "bank_bytes": bank_bytes, "descriptor_limit": descriptor_limit,
            "stripe_exponent": stripe_exponent, "alignment_mask": alignment_mask,
            "metadata_extra": metadata_extra, "metadata_pool": 0, "frame_pool": 0,
            "core_error_flags": 0,
            "frame_flags": [0] * 63, "assigned": [99] * 63, "ppb_frames": [0] * 34,
            "recent_ppb": 99, "frame_word124": [0] * 63,
            "release_request": {"head": 0, "tail": 0, "slots": [0] * 64},
            "delivery_ring": {"read": 2, "write": 2, "slots": [0] * 64},
            "return_ring": {"read": 2, "write": 2, "slots": [0] * 64}}


def _ppb_bank_copy_state(state):
    """Validate the finite model storage, without inventing native index guards."""
    if not isinstance(state, dict):
        raise FormatError("PPB bank model state must be an object")
    expected = set(_ppb_bank_state([], 0))
    if set(state) != expected:
        raise FormatError("PPB bank model state fields do not match")
    result = {}
    for key, count, maximum in (("flags", 34, 65535), ("frame_flags", 63, 65535),
                                ("assigned", 63, 255), ("ppb_frames", 34, 255),
                                ("frame_word124", 63, 0xffffffff)):
        values = state[key]
        if not isinstance(values, list) or len(values) != count or any(
                type(value) is not int or not 0 <= value <= maximum for value in values):
            raise FormatError(f"PPB bank model {key} storage does not match")
        result[key] = list(values)
    banks = state["banks"]
    if not isinstance(banks, list) or len(banks) > 9:
        raise FormatError("PPB bank model bank count does not match")
    result["banks"] = []
    for bank in banks:
        if not isinstance(bank, dict) or set(bank) != {"base", "mask", "stride", "geometry"}:
            raise FormatError("PPB bank model bank fields do not match")
        result["banks"].append({key: _ppb_bank_u32(value, key) for key, value in bank.items()})
    for key in ("bank_bytes", "metadata_pool", "frame_pool", "core_error_flags"):
        result[key] = _ppb_bank_u32(state[key], key)
    for key, maximum in (("descriptor_limit", 34), ("stripe_exponent", 255),
                         ("alignment_mask", 255), ("recent_ppb", 255)):
        value = state[key]
        if type(value) is not int or not 0 <= value <= maximum:
            raise FormatError(f"PPB bank model {key} does not match")
        result[key] = value
    if type(state["metadata_extra"]) is not bool:
        raise FormatError("PPB bank model metadata flag does not match")
    result["metadata_extra"] = state["metadata_extra"]
    for key in ("release_request", "delivery_ring", "return_ring"):
        ring = state[key]
        indices = ("head", "tail") if key == "release_request" else ("read", "write")
        if not isinstance(ring, dict) or set(ring) != set(indices) | {"slots"}:
            raise FormatError("PPB bank model ring fields do not match")
        if any(type(ring[index]) is not int or not 0 <= ring[index] < 64 for index in indices):
            raise FormatError("PPB bank model ring position does not match")
        if not isinstance(ring["slots"], list) or len(ring["slots"]) != 64:
            raise FormatError("PPB bank model ring storage does not match")
        result[key] = {index: ring[index] for index in indices}
        result[key]["slots"] = [_ppb_bank_u32(value, "ring word") for value in ring["slots"]]
    return result


def _ppb_bank_elf_context(payload):
    """Selected original ELF mapping and relocation receipts after all pins."""
    base = 0x2ea60
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", payload, base)
    if (header[0][:7] != b"\x7fELF\x01\x01\x01" or header[2] != 45 or
            header[6] != 0x4aae0 or header[11:] != (40, 55, 54)):
        raise FormatError("PPB bank original ELF header does not match")
    sections = [struct.unpack_from("<10I", payload, 0x79540 + index * 40) for index in range(55)]
    names = bounded(payload, 0x79098, 1191, "PPB bank section names")

    def string(table, offset):
        if offset >= len(table) or table.find(b"\0", offset) < 0:
            raise FormatError("PPB bank original string offset does not match")
        return table[offset:table.index(b"\0", offset)].decode("ascii")

    expected = {2: (".core_critical_code_slice", 0x4000, 0x444),
                4: (".core_critical_code_picture", 0x7f8c, 0x43d0),
                16: (".text", 0x23d74, 0x17ea8)}
    mappings = []
    for index, (name, address, offset) in expected.items():
        section = sections[index]
        if (string(names, section[0]) != name or section[1] != 1 or
                (section[3], section[4]) != (address, offset)):
            raise FormatError("PPB bank containing section does not match")
        mappings.append({"section_index": index, "name": name, "elf_virtual_address": address,
                         "elf_file_offset": offset, "blob_file_offset": base + offset,
                         "size": section[5], "blob_minus_elf_address": base + offset - address})
    symbol_names = bounded(payload, 0x67a95, 8410, "PPB bank symbol names")
    symbols = []
    for offset in range(0x69b70, 0x69b70 + 13392, 16):
        name, value, size, info, other, section = struct.unpack_from("<IIIBBH", payload, offset)
        symbols.append({"name": string(symbol_names, name), "elf_value": value,
                        "size": size, "info": info, "other": other, "section_index": section})
    for name, index, start, end, offset, _ in _PPB_BANK_BODIES:
        section = sections[index]
        if (start < section[3] or end > section[3] + section[5] or
                base + section[4] + start - section[3] != offset or
                not any(symbol["name"] == name and symbol["elf_value"] == start and
                        symbol["section_index"] == index and symbol["size"] ==
                        (0 if name == "_udivmod" else end - start) for symbol in symbols)):
            raise FormatError(f"PPB bank original body mapping {name} does not match")
    dependencies, count = [], 0
    for index, owner, offset, size in ((37, 2, 0x6d020, 1008), (39, 4, 0x6da34, 3132),
                                       (51, 16, 0x72780, 26052)):
        section = sections[index]
        if (section[1], base + section[4], section[5], section[6], section[7], section[9]) != (
                4, offset, size, 35, owner, 12):
            raise FormatError("PPB bank original relocation section does not match")
        for cursor in range(offset, offset + size, 12):
            count += 1
            if count > MAX_PPB_BANK_RELOCATIONS:
                raise FormatError("PPB bank relocation budget exceeded")
            location, info, addend = struct.unpack_from("<IIi", payload, cursor)
            symbol_index, kind = info >> 8, info & 255
            if symbol_index >= len(symbols) or kind not in (0, 4, 6, 7):
                raise FormatError("PPB bank original relocation kind/index does not match")
            owners = [name for name, sec, start, end, _, _ in _PPB_BANK_BODIES
                      if sec == owner and start <= location < end]
            if not owners:
                continue
            if len(owners) != 1 or location + 4 > sections[owner][3] + sections[owner][5]:
                raise FormatError("PPB bank original relocation ownership does not match")
            symbol = symbols[symbol_index]
            target = symbol["elf_value"] + addend
            target_section = symbol["section_index"]
            target_offset = None
            if target_section < len(sections):
                selected = sections[target_section]
                if (0 <= target <= 0xffffffff and selected[1] not in (0, 8) and
                        selected[2] & 2 and selected[3] <= target < selected[3] + selected[5]):
                    target_offset = base + selected[4] + target - selected[3]
            dependencies.append({"owner": owners[0], "elf_virtual_address": location,
                                 "blob_file_offset": base + sections[owner][4] + location - sections[owner][3],
                                 "relocation_record_blob_file_offset": cursor, "type": kind,
                                 "symbol_index": symbol_index, "symbol": symbol["name"],
                                 "symbol_elf_value": symbol["elf_value"], "addend": addend,
                                 "original_target_elf_value": target,
                                 "original_target_is_u32": 0 <= target <= 0xffffffff,
                                 "target_section_index": target_section,
                                 "original_target_blob_file_offset": target_offset,
                                 "runtime_application_validated": False})
    if count != 2516:
        raise FormatError("PPB bank complete relocation context does not match")
    return mappings, dependencies, count


def _ppb_bank_flag_gates(flag):
    if type(flag) is not int or not 0 <= flag <= 65535:
        raise FormatError("PPB bank flag must be a u16")
    eligible = (flag & 0x8800) == 0x8800
    return {"allocator_free": not bool(flag & 0x8000),
            "video_candidate": bool(flag & 0xe000),
            "deallocation_admitted": bool(flag & 0x8000) and not bool(flag & 0x6000),
            "stop_eligible": eligible, "stop_force_release": eligible and not bool(flag & 0x1000)}


def _ppb_bank_contract(payload):
    """Private, pinned selected-state contract; never a host-plane lease API."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("PPB bank payload identity/size does not match")
    total = sum(size for _, _, size, _ in _PPB_BANK_REGIONS)
    if (len(_PPB_BANK_REGIONS) > MAX_PPB_BANK_REGIONS or total > MAX_PPB_BANK_BYTES or
            MAX_PPB_BANK_RELOCATIONS < 1 or MAX_PPB_BANK_MODEL_STEPS < 1):
        raise FormatError("PPB bank validation budget exceeded")
    validated = []
    # All complete selected bodies and metadata, including skipped branches,
    # pass their independent fuses before a single field is interpreted.
    for role, offset, size, digest in _PPB_BANK_REGIONS:
        data = bounded(payload, offset, size, "PPB bank pinned region")
        if hashlib.sha256(data).hexdigest() != digest:
            raise FormatError(f"PPB bank region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset, "size": size, "sha256": digest})
    sections, dependencies, relocation_count = _ppb_bank_elf_context(payload)
    reference_edges = []
    for role, owner, section, call, store, clear in _PPB_BANK_RELEASE_EDGES:
        matches = [entry for entry in dependencies if entry["elf_virtual_address"] == call and
                   entry["owner"] == owner and entry["type"] == 6 and
                   entry["symbol"] == "Core_DeallocatePPB" and
                   entry["original_target_elf_value"] == 0x907c]
        same_section = section == 4
        if len(matches) != (0 if same_section else 1):
            raise FormatError("PPB bank selected deallocation dependency does not match")
        delta = next(entry["blob_minus_elf_address"] for entry in sections if entry["section_index"] == section)
        word = struct.unpack_from("<I", payload, call + delta)[0]
        displacement = (word >> 7) & 0xfffff
        if displacement & (1 << 19):
            displacement -= 1 << 20
        if word & 0xf800007f != 0x28000020 or call + 4 + displacement * 4 != 0x907c:
            raise FormatError("PPB bank pinned direct BL.d dependency does not match")
        reference_edges.append({"role": role, "owner": owner, "section_index": section,
                                "call_elf_virtual_address": call, "call_blob_file_offset": call + delta,
                                "store_elf_virtual_address": store, "store_blob_file_offset": store + delta,
                                "clear_mask": clear, "store_before_callee": True,
                                "normal_delay_slot_store": store == call + 4,
                                "original_relocation_record_present": not same_section,
                                "direct_target_elf_virtual_address": 0x907c})
    return {
        "basis": {"model": "selected-ppb-bank-v1", "conditional": True, "outer_blob_file_offset": 0x2ea60,
                  "complete_body_count": len(_PPB_BANK_BODIES), "code_bytes": 15712,
                  "region_count": len(validated), "validated_bytes": total,
                  "address_spaces": "ELF virtual, bundled blob-file, ARC-local and host DRAM addresses are distinct."},
        "validated_regions": validated, "containing_sections": sections,
        "original_relocation_dependencies": {"complete_table_record_count": relocation_count,
                                             "selected_body_records": dependencies,
                                             "types_are_original_records_not_runtime_ISA_proof": True},
        "context_initialization": {
            "open_bank_count_maximum": 9, "open_allows_zero_banks": True,
            "open_bank_count_check_elf_virtual_addresses": [0x24878, 0x2487c],
            "open_rounding_elf_range": [0x2497c, 0x249a4], "rounding": "u32(value+4095)&0xfffff000",
            "pre_round_nonzero_implies_post_round_nonzero": False,
            "constructor_bank_count_guard": False, "constructor_saved_bank_count_offset": 0x490,
            "constructor_saved_bank_bytes_offset": 0x48c, "constructor_saved_bank_start_offset": 0x3fc,
            "constructor_saved_flags_offset": 0x354, "constructor_cleared_flag_count": 34,
            "constructor_cleared_metadata_field_offset": 68, "metadata_record_bytes": 228,
            "bank_base_increment": "u32(base+bank_bytes)",
            "activation": {"lookup_elf_virtual_address": 0x9fa8, "saved_context_load_elf_virtual_address": 0x9fbc,
                           "copy_call_elf_virtual_address": 0x9fd0, "copy_bytes": 0x5bc,
                           "local_common_header_base": 0x3fffcd70, "snapshot_destination": 0x3fffcdac,
                           "common_header_bytes_not_copied": 60, "successful_coherent_copy_assumed": True},
            "init_geometry": {"stripe_exponent_core_byte_offset": 26,
                              "stripe_exponent_expression": "(init request word3+5)&255",
                              "alignment_mask_core_byte_offset": 27,
                              "alignment_masks_for_request_word2": [63, 127, 255], "default_alignment_mask": 63,
                              "metadata_extra_saved_context_byte_offset": 0x82,
                              "metadata_extra_open_request_byte_offset": 52}},
        "bank_layout": {"arc_local_base": 0x3fffd170, "bank_count_address": 0x3fffd23c,
                        "bank_count_bytes": 1, "bank_count_load_elf_virtual_address": 0xae64,
                        "bank_bytes_address": 0x3fffd238, "bank_bytes_bytes": 4,
                        "bank_bytes_load_elf_virtual_address": 0xaf78, "entry_bytes": 16,
                        "entry_offsets_from_local_base": {"base": 56, "mask": 60, "stride": 64, "geometry": 68},
                        "encoding_bank_bits": 4, "encoding_subslot_bits": 5,
                        "geometry_fields": {"width": [0, 11], "height": [11, 11], "capacity": [22, 6]},
                        "geometry_pack_masks_inputs": False, "final_free_preserves_base": True},
        "descriptor_layout": {"flags_address": 0x3fffd100, "flag_bytes": 2, "flag_count": 34,
                              "allocator_limit_byte_address": 0x3fffceb2, "allocator_limit_is_dynamic": True,
                              "release_native_index_range": [0, 33], "getter_native_index_guard": False,
                              "getter_shifted_index_wraps_u32": True, "metadata_pool_address": 0x3fffd0e8,
                              "metadata_record_bytes": 228, "generation_field_validated": False,
                              "frame_flag_address": 0x3fffce32, "frame_flag_count": 63,
                              "frame_record_bytes": 284, "frame_prefix_bytes": 56,
                              "producer_pool_pointer_address": 0x3fffcfc8,
                              "reader_pool_pointer_address": 0x3fffd2dc,
                              "runtime_pool_identity_validated": False},
        "geometry_path": {"video_parameters_elf_range": [0x80dc, 0x8218],
                          "stripe_height_elf_range": [0xbd90, 0xbdc4], "division_elf_range": [0xbc04, 0xbd18],
                          "stripe_rule": "u32 ceil to stripe; force odd stripe count; unsigned cap1120; store/reloadu16",
                          "chroma_height_input": "signed ASR(height,1)",
                          "vendor_mul16_elf_virtual_addresses": [0x814c, 0x8170],
                          "mul16_supported_conditional_operand_maximum": 32767,
                          "vendor_mul16_semantics_validated": False, "shift_count_at_least32_supported": False,
                          "frame_bytes_rule": "page_round(page_round(Y product)+raw chroma product), plus optional page-rounded metadata",
                          "extra_bytes_rule": "u32(6*((signed width>>4)*(signed height>>4)))",
                          "division_zero_quotient": 0x7fffffff, "division_zero_remainder": 0,
                          "capacity_clamp_is_signed_ge": True, "geometry_input_pack_is_unmasked": True},
        "operations": {
            "allocate": {"free_test_mask": 0x8000, "both_zero_flag": 0xac00, "regular_flag": 0xe800,
                         "capacity32_full_mask": 0xffffffff, "first_free_slot_has_no_native_bound": True},
            "video_address": {"any_flag_mask": 0xe000, "required_bitmap_bit": True,
                              "result": "u32(bank.base+bank.stride*subslot)", "native_extent_guard": False,
                              "special_ac00_can_alias_bank0_slot0": True},
            "deallocate": {"blocking_reference_mask": 0x6000, "required_live_mask": 0x8000,
                           "skip_bank_mask": 0x0400, "metadata_release_callees": ["Core_ReleaseUD", "Core_ReleaseOffsetMeta"],
                           "opaque_metadata_return_errors_checked": False},
            "start": {"clear_flag_mask": 0x0800, "ppb_count": 34, "frame_flag_clear_count": 63,
                      "frame_record_word124_clear_count": 63, "banks_preserved": True,
                      "state_is_after_picture_scan_reset": True,
                      "picture_scan_reset_and_other_effects_modeled": False},
            "stop_selected": {"required_flag_mask": 0x8800, "clear_flag_mask": 0x4000,
                              "force_release_only_when_original_flag1000_clear": True,
                              "state_is_after_prelude": True,
                              "prelude": "MonitorIL, UpdateReleaseQueue and conditional GetUndelivered effects must already be applied; not modeled as no-ops."},
            "step_scope": "Conditional selected bank/flag/record/ring effects, not execution of every instruction in the35 pinned bodies."},
        "reference_edges": reference_edges,
        "delivery_edges": {
            "assignment_getter_call_elf_virtual_address": 0xb23c, "inner_packet_bank_base_word_byte_offset": 28,
            "inner_packet_pointer_in_frame_prefix_byte_offset": 28,
            "assignment_drop2000_condition_frame_flag_bit": 14,
            "attempt_release_call_elf_virtual_address": 0xacb0, "later_il_busy_read_elf_virtual_address": 0xad18,
            "later_il_start_write_elf_virtual_address": 0xad40, "drop4000_is_decode_completion": False,
            "monitor_frame_flag_clear_mask": 0x2400,
            "display_skip_getter_picture_flag_mask": 0x100,
            "display_bank_base_record_byte_offset": 4, "display_chroma_add_record_byte_offset": 8,
            "display_optional_nonzero_add_record_byte_offset": 112,
            "display_dma_write_elf_virtual_address": 0xb8fc, "display_dma_sync_elf_virtual_address": 0xb904,
            "display_record_bytes": 228, "display_queue_put_elf_virtual_address": 0xb980,
            "display_deliver_call_elf_virtual_address": 0xb988, "display_set1000_store_elf_virtual_address": 0xb9a4,
            "circ_buffer_header_word_indices": [0, 1], "circ_buffer_data_word_index_range": [2, 63],
            "release_request_positions": 64, "release_request_wrap_mask": 63,
            "returned_or_delivered_metadata_address_is_host_plane_lease": False,
            "selected_return_indices_are_inputs_not_completion_oracle": True},
        "empty_picture_path": {"zero_dimensions_elf_virtual_addresses": [0xa718, 0xa720],
                               "populate_empty_call_elf_virtual_address": 0xa774,
                               "metadata_tag100_store_elf_virtual_address": 0x26420,
                               "frame_prefix_publication_elf_virtual_address": 0xa7a8,
                               "metadata_publication_elf_virtual_address": 0xa7d4,
                               "display_tag_guard_elf_virtual_addresses": [0xb878, 0xb87c],
                               "selected_empty_producer_tags_picture": True,
                               "all_both_zero_codec_outputs_tagged": False},
        "assumptions": [
            "Pinned original per-section ELF mapping/relocations are source evidence, not runtime relocation or active channel/context identity.",
            "GNU ARC base-case operands, normal both-path delay and taken-only .jd delay, flag and carry effects agree with this vendor ISA; not independently validated.",
            "Vendor mul16 equals the selected product for operands0..32767; larger operands and variable shifts>=32 are unsupported model domains, not proven firmware rejection.",
            "Valid same-channel initialized context and ordinary disjoint local/context/frame/metadata/ring/bank spans; aliasing opaque writes do not alter modeled state.",
            "Selected activation/DMA/local-copy/clear operations complete coherently and preserve modeled words; caches, worklists and active pool identity are not proven.",
            "Opaque codec callbacks, indirect callees, logging, MMIO and metadata-release helpers preserve the selected equations/fields when returning; their complete bodies/effects are not validated.",
            "Selected transitions are serialized snapshots with no concurrent unmodeled mutation of their state; event ordering does not establish asynchronous firmware completion or a reusable lease.",
            "Ready assignment/display branches and returned/undelivered ring inputs are supplied explicitly; STOP state is after its monitor/ring prelude, not an idle-state guarantee.",
            "The tagged empty producer path is selected; other codec-generated both-zero pictures and Assignment getter exclusion are not universally proven."],
        "validation_scope": {"complete_selected_body_pins": True, "selected_state_projection": True,
                             "all_nine_direct_deallocation_edges": True, "original_section_mapping": True,
                             "whole_body_execution": False, "vendor_ISA": False, "runtime_relocation": False,
                             "runtime_context_identity": False, "source_plane_host_ownership": False,
                             "completion_or_cache_coherence": False, "generation_safe_reuse": False,
                             "minimum_inner_ABI": False, "standalone_raw_feed": False,
                             "silicon_incapability": False}}


def _ppb_bank_decode_ledger(payload, words):
    """Decode only the captured flag/bank projection; never acquire a surface.

    Input is 17 flag DWORDs, nine four-DWORD rows, bank bytes and the DWORD
    containing the byte-sized bank count, in original little-endian order.
    The caller supplies capture coherence/identity evidence separately. No
    missing context, queue, completion or generation fields are synthesized.
    """
    contract = _ppb_bank_contract(payload)
    if not isinstance(words, (tuple, list)) or len(words) != 55:
        raise FormatError("PPB ledger requires exactly 55 captured DWORDs")
    raw = [_ppb_bank_u32(word, "ledger word") for word in words]
    count_word = raw[54]
    bank_count = count_word & 255
    if bank_count > contract["context_initialization"]["open_bank_count_maximum"]:
        raise FormatError("PPB ledger bank count exceeds the selected stock OPEN domain")
    flags = [part for word in raw[:17] for part in (word & 65535, word >> 16)]
    banks = [dict(zip(("base", "mask", "stride", "geometry"), raw[17 + 4 * i:21 + 4 * i]))
             for i in range(9)]
    entries = []
    for index, flag in enumerate(flags):
        bank, slot = flag & 15, (flag >> 4) & 31
        candidate = bool(flag & contract["operations"]["video_address"]["any_flag_mask"])
        bitmap_present = bool(banks[bank]["mask"] & (1 << slot)) if bank < 9 else None
        address, wrapped = (0, False) if not candidate else (None, None)
        if candidate and bank < 9:
            # The native getter does not check the declared bank count. Keep
            # that fact separate instead of inventing a firmware bounds guard.
            full = banks[bank]["base"] + banks[bank]["stride"] * slot
            address = full & 0xffffffff if bitmap_present else 0
            wrapped = full > 0xffffffff if bitmap_present else False
        entries.append({"index": index, "flag": flag, "bank": bank, "subslot": slot,
                        "bank_declared": bank < bank_count, "bitmap_present": bitmap_present,
                        "video_candidate": candidate, "video_address_u32": address,
                        "address_wrapped": wrapped})
    return {"flags": flags, "banks": banks, "entries": entries,
            "bank_bytes": raw[53], "bank_count": bank_count,
            "bank_count_word": count_word, "bank_count_upper24_opaque": count_word >> 8,
            "validation_scope": {"pinned_selected_layout": True, "captured_fields_only": True,
                                 "runtime_context_identity": False, "coherent_capture": False,
                                 "physical_source_lease": False, "generation_safe_reuse": False}}


def decode_source_surface_256x96(words):
    """Unpack one measured, finite MEM_RD layout into Y/Cb/Cr bytes.

    Input is exactly 12,288 normalized numeric DWORDs, not serialized bytes
    or an address. The observed profile is 8-bit 256x96 YUV420: 64-byte
    stripes with 96 rows, MSB-first DWORD lanes, and adjacent Cb/Cr samples.
    Chroma uses only 48 rows of each 96-row stripe. Its 12,288 padding bytes
    are ignored; no content pattern or padding value is required.

    This is an offline layout decoder, not device I/O, a generic geometry
    rule, source identity, retention, completion, or an allocation API.
    """
    if type(words) not in (tuple, list) or len(words) != 12288:
        raise FormatError("source layout requires exactly 12288 normalized DWORDs")
    raw = b"".join(_ppb_bank_u32(word, "surface word").to_bytes(4, "big")
                   for word in words)
    y = bytes(raw[(x // 64) * 6144 + row * 64 + x % 64]
              for row in range(96) for x in range(256))
    cb = bytes(raw[24576 + (x // 32) * 6144 + row * 64 + 2 * (x % 32)]
               for row in range(48) for x in range(128))
    cr = bytes(raw[24576 + (x // 32) * 6144 + row * 64 + 2 * (x % 32) + 1]
               for row in range(48) for x in range(128))
    return y, cb, cr


def _ppb_bank_step(contract, state, operation, **args):
    """Finite conditional raw transitions; input state is never modified.

    Caller-selected branch/ring inputs are evidence prerequisites. This is not
    an ARC interpreter, device command, whole-function or asynchronous model.
    The private dictionary checks are schema consistency, not authentication of
    an external caller's proof or state; no public API uses these helpers.
    reference_drop is post-guard; no_display applies its selected frame guards,
    without proving scheduler reachability or complete opaque callee effects.
    """
    if (not isinstance(contract, dict) or not isinstance(contract.get("basis"), dict) or
            not isinstance(contract.get("validation_scope"), dict) or
            contract["basis"].get("model") != "selected-ppb-bank-v1" or
            not contract["validation_scope"].get("selected_state_projection")):
        raise FormatError("PPB bank model requires the selected pinned contract")
    if MAX_PPB_BANK_MODEL_STEPS < 1:
        raise FormatError("PPB bank model budget exceeded")
    schema = {"geometry": ({"width", "height"}, set()), "allocate": ({"width", "height"}, set()),
              "video_address": ({"index"}, set()), "deallocate": ({"index"}, set()),
              "release": ({"index"}, set()), "start": (set(), set()), "stop_selected": (set(), set()),
              "reference_drop": ({"index", "caller"}, set()),
              "no_display": ({"frame"}, set()),
              "assignment_reference": ({"index", "frame_flags"}, set()),
              "display_publish": ({"index", "record"}, {"discarded"}),
              "empty_picture": (set(), set()), "constructor": ({"base", "count", "bank_bytes"}, set()),
              "ring_put": ({"ring", "value"}, set()), "ring_get": ({"ring"}, set()),
              "ppb_from_address": ({"address"}, set()),
              "undelivered": ({"delivery_addresses", "return_addresses"}, set()),
              "order_release": ({"frame", "reason"}, set()),
              "monitor_complete": ({"frame"}, set())}
    if not isinstance(operation, str) or operation not in schema:
        raise FormatError("unsupported PPB bank model operation")
    required, optional = schema[operation]
    if not required <= set(args) or not set(args) <= required | optional:
        raise FormatError("PPB bank model operation arguments do not match")
    current = _ppb_bank_copy_state(state)
    events, steps = [], 0
    u32 = lambda value: value & 0xffffffff

    def event(kind, **fields):
        nonlocal steps
        steps += 1
        if steps > MAX_PPB_BANK_MODEL_STEPS:
            raise FormatError("PPB bank model budget exceeded")
        events.append({"kind": kind, **fields})

    def index(value):
        if type(value) is not int or not 0 <= value < 34:
            raise FormatError("PPB bank index is outside bounded model storage, not a native getter guard")
        return value

    def frame(value):
        if type(value) is not int or not 0 <= value < 63:
            raise FormatError("PPB bank frame index is outside bounded model storage")
        return value

    def selected_bank(flag):
        bank_index, subslot = flag & 15, (flag >> 4) & 31
        if bank_index >= len(current["banks"]):
            raise FormatError("PPB bank descriptor selects unmodeled bank storage; no native bounds guard")
        return current["banks"][bank_index], bank_index, subslot

    def address(ppb):
        flag = current["flags"][index(ppb)]
        if not _ppb_bank_flag_gates(flag)["video_candidate"]:
            return 0
        bank, _, slot = selected_bank(flag)
        return u32(bank["base"] + u32(bank["stride"] * slot)) if bank["mask"] & (1 << slot) else 0

    def deallocate(ppb):
        ppb = index(ppb)
        flag = current["flags"][ppb]
        if not _ppb_bank_flag_gates(flag)["deallocation_admitted"]:
            return False
        # Contents/callee effects at record+68/+224 are intentionally opaque.
        event("metadata_release", index=ppb, record_address=u32(current["metadata_pool"] + 228 * ppb),
              conditional_nonzero_fields=[68, 224], callees=["Core_ReleaseUD", "Core_ReleaseOffsetMeta"],
              complete_callee_effects_modeled=False)
        if not flag & 0x0400:
            bank, bank_index, slot = selected_bank(flag)
            bank["mask"] &= u32(~(1 << slot))
            event("bank_mask", bank=bank_index, value=bank["mask"])
            if bank["mask"] == 0:
                bank["geometry"] = bank["stride"] = 0
                event("bank_empty", bank=bank_index, base_preserved=bank["base"])
        current["flags"][ppb] = 0
        event("flag", index=ppb, value=0)
        return True

    def drop(ppb, caller):
        ppb = index(ppb)
        selected = [edge for edge in _PPB_BANK_RELEASE_EDGES if edge[0] == caller]
        if len(selected) != 1:
            raise FormatError("PPB bank reference caller is not one of the nine pinned edges")
        clear = selected[0][5]
        current["flags"][ppb] &= ~clear
        event("reference_drop", index=ppb, caller=caller, clear_mask=clear, value=current["flags"][ppb])
        return deallocate(ppb)

    def from_address(value):
        value = _ppb_bank_u32(value, "metadata address")
        for ppb in range(34):
            if value == u32(current["metadata_pool"] + 228 * ppb):
                return ppb
        return -1

    def ring_named(name):
        if name not in ("delivery_ring", "return_ring"):
            raise FormatError("PPB bank circular ring selector does not match")
        return current[name]

    def put(ring, value):
        value = _ppb_bank_u32(value, "ring value")
        if not 2 <= ring["read"] <= 63 or not 2 <= ring["write"] <= 63:
            raise FormatError("PPB bank circular put would enter opaque Debug/out-of-domain storage")
        slot = ring["write"]
        ring["slots"][slot] = value
        ring["write"] = 2 if slot == 63 else slot + 1
        event("ring_put", data_word_index=slot, value=value, next_write=ring["write"], native_full_guard=False)
        return 0

    if operation == "geometry":
        result = _ppb_bank_geometry(args["width"], args["height"], current["stripe_exponent"],
                                    current["alignment_mask"], current["metadata_extra"])
    elif operation == "constructor":
        base, size = _ppb_bank_u32(args["base"], "bank base"), _ppb_bank_u32(args["bank_bytes"], "bank bytes")
        count = args["count"]
        if type(count) is not int or not 0 <= count <= 9:
            raise FormatError("PPB bank constructor count requires the OPEN0..9 model premise")
        current["banks"] = [{"base": u32(base + size * bank), "mask": 0, "stride": 0, "geometry": 0}
                            for bank in range(count)]
        current["bank_bytes"] = size
        current["flags"] = [0] * 34
        current["core_error_flags"] = 0  # C+64 is inside the assumed context clear.
        event("constructor_projection", banks=count, cleared_flags=34, cleared_metadata_field68_count=34,
              opaque_clear_and_context_effects_modeled=False)
        result = None
    elif operation == "allocate":
        width, height = _ppb_bank_u32(args["width"], "width"), _ppb_bank_u32(args["height"], "height")
        ppb = next((value for value in range(current["descriptor_limit"])
                    if _ppb_bank_flag_gates(current["flags"][value])["allocator_free"]), None)
        result = -1
        if ppb is not None and width == height == 0:
            current["flags"][ppb] = 0xac00
            event("flag", index=ppb, value=0xac00)
            result = ppb
        elif ppb is not None:
            choice = None
            for bank_index, bank in enumerate(current["banks"]):
                geometry = bank["geometry"]
                if not geometry or (geometry & 2047) != width or ((geometry >> 11) & 2047) != height:
                    continue
                capacity = (geometry >> 22) & 63
                if capacity >= 32 and capacity != 32:
                    raise FormatError("PPB bank capacity shift>=32 has unvalidated vendor behavior")
                full = 0xffffffff if capacity == 32 else (1 << capacity) - 1
                if bank["mask"] & full != full:
                    choice = bank_index
                    break
            if choice is None:
                choice = next((value for value, bank in enumerate(current["banks"]) if not bank["geometry"]), None)
                if choice is not None:
                    geometry = _ppb_bank_geometry(width, height, current["stripe_exponent"],
                                                 current["alignment_mask"], current["metadata_extra"])
                    if geometry["frame_bytes"] > current["bank_bytes"]:
                        current["core_error_flags"] |= 0x400
                        event("core_error_flags", value=current["core_error_flags"])
                        event("frame_exceeds_bank", frame_bytes=geometry["frame_bytes"], bank_bytes=current["bank_bytes"])
                        choice = None
                    else:
                        current["core_error_flags"] &= ~0x400
                        event("core_error_flags", value=current["core_error_flags"])
                        bank = current["banks"][choice]
                        capacity = _ppb_bank_capacity(current["bank_bytes"], geometry["frame_bytes"])
                        bank["geometry"] = u32(width | u32(height << 11) | u32(capacity["capacity"] << 22))
                        bank["stride"], bank["mask"] = geometry["frame_bytes"], 0
                        event("bank_geometry", bank=choice, geometry=bank["geometry"], stride=bank["stride"],
                              capacity=capacity["capacity"], quotient=capacity["quotient"])
            if choice is not None:
                bank = current["banks"][choice]
                slot = next((value for value in range(32) if not bank["mask"] & (1 << value)), None)
                if slot is None:
                    raise FormatError("PPB bank unbounded native slot search exceeds model domain")
                bank["mask"] |= 1 << slot
                current["flags"][ppb] = (0xe800 | (slot << 4) | choice) & 65535
                event("bank_mask", bank=choice, value=bank["mask"])
                event("flag", index=ppb, value=current["flags"][ppb])
                result = ppb
    elif operation == "video_address":
        result = address(args["index"])
    elif operation == "deallocate":
        result = deallocate(args["index"])
    elif operation == "release":
        value = _ppb_bank_u32(args["index"], "release index")
        # The selected Core_ReleasePPB has a real signed0..33 guard. The
        # unchecked getter/deallocator do not inherit that guard.
        result = drop(value, "release") if value < 34 else False
    elif operation == "reference_drop":
        result = drop(args["index"], args["caller"])
    elif operation == "no_display":
        value = frame(args["frame"])
        original = current["frame_flags"][value]
        updated = original & 0x77ff
        if original & 0x0200:
            updated |= 0x4000
        elif current["assigned"][value] != 99:
            drop(current["assigned"][value], "no_display")
        current["frame_flags"][value] = updated
        event("no_display_frame_flag", frame=value, value=updated)
        word = current["frame_word124"][value]
        if word:
            current["frame_word124"][value] = 0
            event("frame_word124_clear", frame=value, value=0,
                  field_address=u32(current["frame_pool"] + 284 * value + 124), dma_and_sync_assumed=True)
            event("opaque_frame_metadata_release", frame=value, callee="Core_ReleaseUD", argument=word,
                  complete_callee_effects_modeled=False)
        # The state has no frame+280 field: do not invent its contents, branch
        # outcome, conditional ReleaseOffsetMeta call or following zero store.
        event("opaque_frame_offset_metadata", frame=value,
              field_address=u32(current["frame_pool"] + 284 * value + 280),
              conditional_nonzero_field=280, callee="Core_ReleaseOffsetMeta", field_contents_modeled=False,
              complete_callee_effects_modeled=False)
        result = None
    elif operation == "assignment_reference":
        ppb, flags = index(args["index"]), args["frame_flags"]
        if type(flags) is not int or not 0 <= flags <= 65535:
            raise FormatError("PPB bank assignment frame flags must be a u16")
        if flags & 0x4000:
            current["flags"][ppb] &= ~0x2000
            event("assignment_reference_drop", index=ppb, clear_mask=0x2000, value=current["flags"][ppb])
        result = current["flags"][ppb]
    elif operation == "start":
        current["flags"] = [flag & ~0x0800 for flag in current["flags"]]
        current["frame_flags"], current["frame_word124"] = [0] * 63, [0] * 63
        # The same CORE+124 word contains control bytes: START writes byte124
        # zero, byte125 one and byte126 zero; byte127 is not written here.
        current["core_error_flags"] = (current["core_error_flags"] & 0xff000000) | 0x100
        event("start_projection", cleared_ppb_mask=0x0800, ppb_count=34, frame_count=63,
              opaque_picture_scan_and_other_state_effects_modeled=False)
        result = None
    elif operation == "stop_selected":
        freed = []
        for ppb, original in enumerate(list(current["flags"])):
            gates = _ppb_bank_flag_gates(original)
            if gates["stop_eligible"]:
                current["flags"][ppb] &= ~0x4000
                event("stop_reference_drop", index=ppb, clear_mask=0x4000, value=current["flags"][ppb])
                if gates["stop_force_release"] and drop(ppb, "release"):
                    freed.append(ppb)
        result = freed
    elif operation == "empty_picture":
        result = {"width": 0, "height": 0, "picture_flags": 0x100, "record_bytes": 228,
                  "selected_producer_only": True}
        event("empty_picture_projection", clear_record_bytes=228, set_picture_flag=0x100)
    elif operation == "display_publish":
        ppb, record = index(args["index"]), args["record"]
        if not isinstance(record, dict) or set(record) != {"flags", "y_offset", "chroma_offset", "optional_offset"}:
            raise FormatError("PPB bank display selected record fields do not match")
        record = {key: _ppb_bank_u32(value, "picture field") for key, value in record.items()}
        discarded = args.get("discarded", False)
        if type(discarded) is not bool:
            raise FormatError("PPB bank display branch selector must be boolean")
        if not record["flags"] & 0x100:
            base = address(ppb)
            record["y_offset"] = base
            record["chroma_offset"] = u32(record["chroma_offset"] + base)
            if record["optional_offset"]:
                record["optional_offset"] = u32(record["optional_offset"] + base)
        event("display_record_publication", index=ppb, record_bytes=228,
              record_address=u32(current["metadata_pool"] + 228 * ppb), dma_and_sync_assumed=True,
              omitted_record_metadata_effects=True)
        if discarded:
            drop(ppb, "discard")
        else:
            put(current["delivery_ring"], u32(current["metadata_pool"] + 228 * ppb))
            event("deliver_picture", is_host_plane_lease=False, opaque_interface_effects_modeled=False)
            current["flags"][ppb] |= 0x1000
            event("flag", index=ppb, value=current["flags"][ppb])
        result = record
    elif operation in ("ring_put", "ring_get"):
        ring = ring_named(args["ring"])
        if operation == "ring_put":
            result = put(ring, args["value"])
        elif ring["read"] == ring["write"] or not 2 <= ring["read"] <= 63 or not 2 <= ring["write"] <= 63:
            result = 0
        else:
            slot = ring["read"]
            result = ring["slots"][slot]
            ring["read"] = 2 if slot == 63 else slot + 1
            event("ring_get", data_word_index=slot, value=result, next_read=ring["read"])
    elif operation == "ppb_from_address":
        result = from_address(args["address"])
    elif operation == "undelivered":
        freed = []
        for key, caller in (("delivery_addresses", "undelivered_display"), ("return_addresses", "undelivered_return")):
            values = args[key]
            if not isinstance(values, list) or len(values) > MAX_PPB_BANK_MODEL_STEPS:
                raise FormatError("PPB bank explicit undelivered inputs exceed the model budget")
            for value in values:
                value = _ppb_bank_u32(value, "selected ring result")
                if value == 0:
                    raise FormatError("PPB bank explicit undelivered list excludes the terminal zero result")
                ppb = from_address(value)
                event("selected_undelivered_ring_result", caller=caller, address=value, index=ppb)
                if ppb >= 0 and drop(ppb, caller):
                    freed.append(ppb)
        result = freed
    elif operation == "order_release":
        value, reason = frame(args["frame"]), args["reason"]
        if type(reason) is not int or not 0 <= reason <= 255:
            raise FormatError("PPB bank release-request reason must be a byte")
        current["frame_flags"][value] |= 0x10
        ring = current["release_request"]
        slot = ring["head"]
        ring["slots"][slot] = value | (reason << 8)
        ring["head"] = (slot + 1) & 63
        event("order_release", frame=value, reason=reason, position=slot, no_immediate_bank_release=True)
        result = None
    else:  # Selected completed-metadata frame flag effect, not its completion oracle.
        value = frame(args["frame"])
        current["frame_flags"][value] &= ~0x2400
        event("monitor_complete_projection", frame=value, clear_frame_mask=0x2400,
              completed_dma_metadata_is_input=True, metadata_word0_effects_modeled=False)
        result = None
    return {"state": current, "result": result, "events": events}


def _ppb_bank_admission(contract, state, width, height):
    """Mathematical safe-image premises, NOT runtime/host ownership evidence."""
    if (not isinstance(contract, dict) or not isinstance(contract.get("basis"), dict) or
            contract["basis"].get("model") != "selected-ppb-bank-v1"):
        raise FormatError("PPB bank admission requires the selected pinned contract")
    current = _ppb_bank_copy_state(state)
    width, height = _ppb_bank_u32(width, "width"), _ppb_bank_u32(height, "height")
    reasons = []
    if not (0 < width <= 2047 and 0 < height <= 2047):
        reasons.append("positive11-bit dimensions required; both-zero special and malformed geometry are separate raw cases")
    if current["stripe_exponent"] >= 32:
        reasons.append("unsupported vendor stripe shift")
    if current["alignment_mask"] not in (63, 127, 255):
        reasons.append("expected INIT alignment mask required")
    if not current["banks"] or not current["bank_bytes"] or not current["descriptor_limit"]:
        reasons.append("positive bank span/count and descriptor limit required")
    if current["bank_bytes"] & 4095:
        reasons.append("OPEN-derived bank bytes must be page aligned")
    geometry = None
    try:
        geometry = _ppb_bank_geometry(width, height, current["stripe_exponent"],
                                      current["alignment_mask"], current["metadata_extra"])
    except FormatError as error:
        reasons.append(str(error))
    if geometry is not None and not 0 < geometry["frame_bytes"] <= current["bank_bytes"]:
        reasons.append("positive image stride fitting bank required")
    if geometry is not None and (geometry["y_stripe_height"] < height or
                                 geometry["chroma_stripe_height"] < (height + 1) // 2):
        reasons.append("requested rows must fit the capped modeled luma/chroma stripes")
    intervals, occupied, expected_masks = [], set(), [0] * len(current["banks"])
    for bank_index, bank in enumerate(current["banks"]):
        start, end = bank["base"], bank["base"] + current["bank_bytes"]
        if not start or end > (1 << 32):
            reasons.append("bank base/span must be nonzero and nonwrapping")
        if start & 4095:
            reasons.append("OPEN-derived bank bases must be page aligned")
        if start != current["banks"][0]["base"] + bank_index * current["bank_bytes"]:
            reasons.append("initialized bank bases must follow the constructor's nonwrapping increment")
        if any(start < other_end and other_start < end for other_start, other_end in intervals):
            reasons.append("bank spans must be disjoint")
        intervals.append((start, end))
        packed, mask, stride = bank["geometry"], bank["mask"], bank["stride"]
        capacity = (packed >> 22) & 63
        if packed == 0:
            if mask or stride:
                reasons.append("empty geometry requires empty mask/stride")
        elif (not 1 <= capacity <= 32 or not (packed & 2047) or not ((packed >> 11) & 2047) or
              not stride or capacity * stride > current["bank_bytes"] or
              (capacity < 32 and mask >> capacity)):
            reasons.append("occupied bank requires positive geometry/capacity/stride and bounded slot extents")
        if packed:
            packed_width, packed_height = packed & 2047, (packed >> 11) & 2047
            if packed != packed_width | (packed_height << 11) | (capacity << 22):
                reasons.append("occupied bank geometry must contain only the positive11-bit dimension/capacity fields")
            try:
                bank_geometry = _ppb_bank_geometry(packed_width, packed_height, current["stripe_exponent"],
                                                   current["alignment_mask"], current["metadata_extra"])
                if (bank_geometry["y_stripe_height"] < packed_height or
                        bank_geometry["chroma_stripe_height"] < (packed_height + 1) // 2):
                    reasons.append("occupied bank rows must fit the capped modeled stripes")
                if stride != bank_geometry["frame_bytes"]:
                    reasons.append("occupied bank stride must match its packed dimensions and active geometry parameters")
                if capacity != _ppb_bank_capacity(current["bank_bytes"], bank_geometry["frame_bytes"])["capacity"]:
                    reasons.append("occupied bank capacity must match the selected division/clamp")
            except FormatError as error:
                reasons.append(str(error))
    for ppb, flag in enumerate(current["flags"]):
        if ppb >= current["descriptor_limit"] and flag & 0x8000:
            reasons.append("live descriptor lies beyond active allocator limit")
        if not flag & 0xe000:
            continue
        if flag & 0x0400:
            reasons.append("no-video descriptor cannot be admitted to ordinary video getter")
            continue
        bank_index, slot = flag & 15, (flag >> 4) & 31
        if not flag & 0x8000 or bank_index >= len(current["banks"]):
            reasons.append("getter-candidate descriptor must be live and select initialized bank")
            continue
        bank = current["banks"][bank_index]
        capacity = (bank["geometry"] >> 22) & 63
        key = (bank_index, slot)
        if key in occupied:
            reasons.append("descriptor bank/subslot ownership must be injective")
        occupied.add(key)
        expected_masks[bank_index] |= 1 << slot
        if not bank["mask"] & (1 << slot) or slot >= capacity:
            reasons.append("descriptor must reference an occupied in-capacity subslot")
    if any(bank["mask"] != expected_masks[value] for value, bank in enumerate(current["banks"])):
        reasons.append("every occupied bank bit requires exactly one live descriptor")
    return {"admitted": not reasons, "reasons": sorted(set(reasons)), "geometry": geometry,
            "conditional_vendor_ISA": True, "runtime_ownership_established": False,
            "host_plane_lease": False, "generation_safe_reuse": False}


def _mfd_source_model(record, rows):
    """Selected A32 arithmetic only; this is not an input allocation ABI."""
    if len(record) != 116:
        raise FormatError("MFD source model requires the selected 116-byte field prefix")
    if tuple(map(tuple, rows)) != ((0, 64, 6), (1, 128, 7), (2, 256, 8)):
        raise FormatError("MFD source model requires the three pinned table rows")
    selector, mode, form, field = record[0x5c], record[8], record[0x27], record[0x28]
    if selector > 2 or form not in (1, 2, 3):
        raise FormatError("MFD source model is conditional on selector 0..2 and format 1..3")
    _, stripe, shift = rows[selector]
    word = lambda offset: struct.unpack_from("<I", record, offset)[0]
    u32 = lambda value: value & 0xffffffff
    sy = stripe if mode == 2 else 2 * stripe
    sc = (stripe if field == 1 else 2 * stripe) if form == 1 else sy
    writes = [[0x540010, u32(sy | (sc << 16))],
              [0x540028, word(0x54)], [0x54002c, word(0x58)]]
    if form == 3:
        return {"writes": writes, "return_value": 8}
    # The pinned address helper establishes arithmetic, not coordinate axes.
    offset_6c = word(0x6c) & (0xfffffffe if form == 1 else 0xffffffff)
    offset_70 = word(0x70) & 0xfffffffe
    quotient, remainder = offset_70 >> shift, offset_70 & (stripe - 1)
    # The wrapped offset_6c product is logically shifted before the chroma
    # format shift; moving wrap to the final addition changes the result.
    product = u32(stripe * offset_6c)
    y = u32(word(0x34) + product +
            (u32(u32(word(0x54) * quotient) * stripe) << 4) + remainder)
    c = u32(word(0x38) + ((product >> 1) << (form - 1)) +
            (u32(u32(word(0x58) * quotient) * stripe) << 4) + remainder)
    if mode == 1:
        y, c = u32(y + stripe), u32(c + stripe)
    writes.extend(([0x54001c, y], [0x540020, c]))
    return {"writes": writes, "return_value": 0}


def _mfd_source_map(payload):
    """Bounded original A32 regions, interpreted under stated call conditions."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("MFD source payload size does not match the bundled baseline")
    if (len(_MFD_SOURCE_REGIONS) > MAX_MFD_SOURCE_REGIONS or
            sum(len(bytes.fromhex(raw)) for _, _, raw in _MFD_SOURCE_REGIONS) > MAX_MFD_SOURCE_BYTES):
        raise FormatError("MFD source region/byte budget exceeded")
    regions = []
    for role, offset, raw in _MFD_SOURCE_REGIONS:
        expected = bytes.fromhex(raw)
        if bounded(payload, offset, len(expected), "MFD source region") != expected:
            raise FormatError(f"MFD source region {role} does not match the baseline")
        regions.append({"role": role, "blob_file_offset": offset, "bytes": len(expected)})
    rows = [list(struct.unpack_from("<3I", payload, 0x2cccc + 12 * index)) for index in range(3)]
    edges = [(0x85f8, 0xe110, True), (0x860c, 0x84cc, False),
             (0x84dc, 0x1bfc, True), (0x1cf0, 0x1918, False)]
    for offset, target, link in edges:
        if _a32_branch(payload, offset, link=link)["target_blob_file_offset"] != target:
            raise FormatError("MFD source handoff branch does not match the baseline")
    for offset, literal, value in ((0x1928, 0x1b28, 0x2cccc),
                                   (0x1998, 0x1b48, 0x540010),
                                   (0x19ac, 0x1b4c, 0x540028),
                                   (0x19bc, 0x1b50, 0x54002c),
                                   (0x1ab4, 0x1b6c, 0x54001c),
                                   (0x1ac4, 0x1b70, 0x540020)):
        decoded = _a32_literal(payload, offset)
        if (decoded["literal_blob_file_offset"], decoded["literal_value"]) != (literal, value):
            raise FormatError("MFD source literal does not match the baseline")
    examples = []
    for index in range(3):
        record = bytearray(116)
        record[8], record[0x27], record[0x5c] = 0, 2, index
        for offset, value in ((0x34, 0x1000), (0x38, 0x8000), (0x54, 40),
                              (0x58, 20), (0x6c, 3), (0x70, 5)):
            struct.pack_into("<I", record, offset, value)
        examples.append({"selector": index, "mode": 0, "format": 2, "field": 0,
                         "y_base": 0x1000, "chroma_base": 0x8000,
                         "luma_nmby": 40, "chroma_nmby": 20, "offset_6c": 3, "offset_70": 5,
                         "conditional_model": _mfd_source_model(record, rows)})
    return {
        "device_observed": False,
        "validation": {"regions": regions, "region_count": len(regions),
                       "bytes": sum(region["bytes"] for region in regions)},
        "source_record_to_picture": {
            "entry_blob_file_offset": 0xe110, "nonnull_record_register": 5,
            "picture_register": 4,
            "final_copies": [
                {"load_blob_file_offset": 0xe19c, "record_word_offset": 4,
                 "store_blob_file_offset": 0xe1a0, "picture_word_offset": 0x34},
                {"load_blob_file_offset": 0xe1a4, "record_word_offset": 8,
                 "store_blob_file_offset": 0xe1a8, "picture_word_offset": 0x38}],
            "return_branch_blob_file_offset": 0xe1ac,
            "return_blob_file_offset": 0xe244,
            "scope": "Final stores on the selected non-null path, after unvalidated callees; not allocation or ownership."},
        "selected_handoff": {
            "record_pointer_stack_offset": 0x14, "picture_stack_offset": 0x20,
            "source_call_blob_file_offset": 0x85f8, "join_blob_file_offset": 0x860c,
            "setup_call_blob_file_offset": 0x84dc,
            "setup_picture_argument_register": 3,
            "setup_picture_saved_register": 4,
            "address_helper_entry_blob_file_offset": 0x1918,
            "address_helper_picture_argument_register": 1,
            "register_context_physical_base_verified": False},
        "addressing": {
            "selected_field_prefix_bytes": 116,
            "field_offsets": {"mode_byte": 8, "format_byte": 0x27, "field_byte": 0x28,
                             "y_base_word": 0x34, "chroma_base_word": 0x38,
                             "luma_nmby_word": 0x54, "chroma_nmby_word": 0x58,
                             "table_selector_byte": 0x5c, "offset_6c_word": 0x6c, "offset_70_word": 0x70},
            "table_blob_file_offset": 0x2cccc, "table_row_bytes": 12, "selected_table_rows": rows,
            "table_selector_checked_by_firmware": False,
            "table_first_word_use": "Diagnostic argument only in the pinned helper.",
            "arithmetic": "A32 unsigned 32-bit wrap at each operation; logical shifts.",
            "model_scope": "Selector 0..2, format byte 1/2/3; numeric modes/flags are not asserted to be legal pixel formats.",
            "write_rdb_order": [0x540010, 0x540028, 0x54002c, 0x54001c, 0x540020],
            "write_helper_entry_blob_file_offset": 0x1e8e8,
            "writes_are_context_relative": True,
            "format3": {"return_value": 8, "preceding_write_count": 3, "line_address_writes": False},
            "conditional_zero_offset_identity": {
                "conditions": "Format 1/2, selected table row, offset_6c=offset_70=0, mode!=1, original bases preserved.",
                "line_address0": "original frame-record word+4", "line_address1": "original frame-record word+8"},
            "model_examples": examples},
        "conditions": [
            "The selected paths execute in A32 with the original code/literals and returning ABI-preserving callees.",
            "The non-null frame record and picture/stack/context/table accesses are valid and stable.",
            "Unvalidated callees and concurrent activity do not alias/mutate the tracked fields or saved registers.",
            "The model restricts the otherwise unchecked selector to the three pinned rows and format to 1/2/3."],
        "limitations": [
            "Selected record words are source bases, not host RX output-descriptor addresses.",
            "This conditional arithmetic is not a complete frame layout, packing/alignment or host submission ABI.",
            "Original words may use firmware-specific address namespaces; register-context physical base is unresolved.",
            "Allocation bounds, source-bank ownership, cache/DMA visibility, release and quiescence remain unvalidated.",
            "No host/device access, firmware execution, raw submission or standalone backend capability is established."]}


def _scaler_fir_map(payload):
    """Pure selected A32 evidence; public entry pins the entire bundled blob."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("scaler FIR payload size does not match the bundled baseline")
    total = sum(size for _, _, size, _ in _SCALER_FIR_REGIONS)
    if len(_SCALER_FIR_REGIONS) > MAX_SCALER_FIR_REGIONS or total > MAX_SCALER_FIR_BYTES:
        raise FormatError("scaler FIR region/byte budget exceeded")
    validated = []
    for role, offset, size, digest in _SCALER_FIR_REGIONS:
        raw = bounded(payload, offset, size, "scaler FIR region")
        if hashlib.sha256(raw).hexdigest() != digest:
            raise FormatError(f"scaler FIR region {role} does not match the baseline")
        validated.append({"role": role, "blob_file_offset": offset,
                          "size_bytes": size, "sha256": digest})

    tables = []
    for axis, offset, count in (("vertical", 0x2cdf0, 32), ("horizontal", 0x2ccf0, 64)):
        raw = bounded(payload, offset, count * 4, "scaler FIR table")
        words = list(struct.unpack("<" + "I" * count, raw))
        # RDB COEFF_even is the high field, COEFF_odd the low field.
        taps = [tap for word in words for tap in ((word >> 18) & 4095, (word >> 2) & 4095)]
        taps_per_phase = count // 4
        phases = []
        for index in range(8):
            unsigned = taps[index * taps_per_phase:(index + 1) * taps_per_phase]
            signed = [tap if tap < 2048 else tap - 4096 for tap in unsigned]
            phases.append({"phase_index": index, "unsigned12_taps": unsigned,
                           "signed12_candidate_taps": signed, "signed12_candidate_sum": sum(signed)})
        tables.append({"name": axis, "axis": axis, "blob_file_offset": offset,
                       "size_bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
                       "words": words, "phases": phases,
                       "reserved_bits_zero": all(word & 0xc003c003 == 0 for word in words)})

    banks = []
    for name, axis, load, table_load, count, loop in (
            ("vertical_luma", "vertical", 0x2378, 0x237c, 32, 0x2380),
            ("vertical_chroma", "vertical", 0x23a0, 0x237c, 32, 0x23a8),
            ("horizontal_luma", "horizontal", 0x23c8, 0x23d0, 64, 0x23d4),
            ("horizontal_chroma", "horizontal", 0x23f4, 0x23d0, 64, 0x23fc)):
        banks.append({"name": name, "axis": axis,
                      "rdb_base_address": _a32_literal(payload, load)["literal_value"],
                      "table_blob_file_offset": _a32_literal(payload, table_load)["literal_value"],
                      "word_count": count, "loop_entry_blob_file_offset": loop})
    calls = [(0x8518, 0x1f8c), (0x8634, 0x1f8c), (0x20fc, 0x21ac), (0x215c, 0x21ac)]
    for offset, target in calls:
        if _a32_branch(payload, offset, link=True)["target_blob_file_offset"] != target:
            raise FormatError("scaler FIR caller does not match the baseline")
    tail = _a32_branch(payload, 0x2474)
    return {
        "schema_version": 1, "isa": "A32", "endianness": "little", "device_observed": False,
        "validation": {"validated_regions": validated, "validated_bytes": total},
        "entry_blob_file_offset": 0x21ac,
        "writer": {"entry_blob_file_offset": 0x1e8e8,
                   "operation": "Store r2 at [*(u32 *)r0 + r1] using the original A32 writer.",
                   "physical_base_validated": False},
        "tables": tables, "banks": banks, "coefficient_write_count": sum(bank["word_count"] for bank in banks),
        "preceding_setup_write_count": 20,
        "format": {"bits": 12, "even_tap_shift": 18, "odd_tap_shift": 2,
                   "even_tap_mask": 0x3ffc0000, "odd_tap_mask": 0x00003ffc,
                   "reserved_mask": 0xc003c003, "signedness_confirmed": False,
                   "fractional_precision_confirmed": False, "normalization_candidate": 1024},
        "enable_tail": {"branch_blob_file_offset": tail["blob_file_offset"],
                        "target_blob_file_offset": tail["target_blob_file_offset"],
                        "rdb_address": _a32_literal(payload, 0x2468)["literal_value"], "value": 1},
        "routing": {
            "entry_blob_file_offset": 0x1f8c, "channel_stride_bytes": 0x1cc,
            "cache_word_offset": 0x1c8, "active_byte_offset": 0x1cc,
            "picture_selector_byte_offset": 8,
            "target_width_fields": {"selector_equal_2": {"shift": 8, "bits": 12},
                                    "selector_other": {"shift": 20, "bits": 12}},
            "source_record_width_word_offset": 12, "reuse_record_flag_mask": 0x100,
            "setup_predicate": "Cached u32 != 0 and (source-record word+12 == 0 or unsigned word+12 > selected target width).",
            "reuse_predicate": "Source-record word+0 bit8 is set and channel active byte equals 1.",
            "picture_dispatch_call_offsets": [0x8518, 0x8634], "setup_call_offsets": [0x20fc, 0x215c],
            "complete_picture_caller_validated": False},
        "open_fields": {
            "entry_blob_file_offset": 0x55d4, "request_word_offset": 0x20, "request_enable_mask": 1,
            "initial_cache_value": "Incoming r9; its initialization lies outside this selected region.",
            "field_input_range_inclusive": [128, 1919], "odd_values_round_up": True,
            "upper_field": {"shift": 20, "bits": 12, "fallback": 960},
            "lower_field": {"shift": 8, "bits": 12, "fallback": 1280},
            "cache_composition": "Enabled path ORs normalized fields and bit0 into initial r9; disabled path stores r9."},
        "conditions": [
            "Selected original paths execute in A32 with valid stable context, record, picture, stack and table storage.",
            "Writer destinations and preceding setup writes do not alias or mutate the coefficient tables or tracked entry/context storage.",
            "Opaque logging/arithmetic callees return and preserve the ABI, saved state and tracked storage; their semantics are not modeled.",
            "The coefficient-loop entry has r6=0 and a stable r7 writer context, as set by the selected setup prefix.",
            "Incoming OPEN r9 must be zero for the selected stores to clear the cache or yield just the normalized fields; that initialization is not pinned here."],
        "limitations": [
            "Little-endian file words and RDB field layout do not prove hardware coefficient signedness, Q precision, rounding or clipping.",
            "Signed-12 two's-complement phase sums of 1024 are an arithmetic candidate, not a validated pixel oracle.",
            "Context-relative register writes do not establish a physical MMIO base or authorize host register access.",
            "Selected caller edges do not prove the complete host route, valid dimensions, returning division helpers or successful setup completion.",
            "Source-bank ownership/lifetime, DMA visibility, clocks, reset and quiescence remain unresolved; no standalone scaler or raw backend execution is proved.",
            "No file/device access or firmware execution occurs in this private mapper; no hardware capability is advertised."]}


def _arm_ppb_metadata_handoff(payload):
    """Exact stock A32 metadata handoff, not a raw-surface ownership API."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("ARM PPB handoff payload size does not match")
    total = sum(size for _, _, size, _ in _ARM_PPB_HANDOFF_REGIONS)
    if len(_ARM_PPB_HANDOFF_REGIONS) > 4 or total > 600:
        raise FormatError("ARM PPB handoff validation budget exceeded")
    regions = []
    for role, offset, size, digest in _ARM_PPB_HANDOFF_REGIONS:
        data = bounded(payload, offset, size, "ARM PPB handoff body")
        if hashlib.sha256(data).hexdigest() != digest:
            raise FormatError(f"ARM PPB handoff {role} body does not match")
        regions.append({"role": role, "blob_file_offset": offset,
                        "bytes": size, "sha256": digest})
    calls = []
    for site, target in ((0xd69c, 0x1fdac), (0xd6ac, 0x203c4),
                         (0xd774, 0x1fdac), (0xd5d4, 0x203c4),
                         (0xd5f8, 0x203c4), (0xd618, 0x203c4)):
        branch = _a32_branch(payload, site, link=True)
        if branch["target_blob_file_offset"] != target:
            raise FormatError("ARM PPB handoff call target does not match")
        calls.append({"site": site, "target": target})
    return {
        "isa": "A32", "regions": regions, "validated_bytes": total,
        "calls": calls, "device_observed": False,
        "record": {"virtual_metadata_word_offset": 0,
                   "physical_metadata_word_offset": 4,
                   "source_plane_layout_validated": False},
        "ring": {"read_word_offset": 0, "write_word_offset": 4,
                 "first_data_byte_offset": 8, "index_range": [2, 63],
                 "slot_expression": "ring + 4*index",
                 "next_index": "index+1, replacing 64 with 2"},
        "acquire": {"entry": 0xd624, "ring_handle_offset": 0x250,
                    "null_ring_returns_without_access": True,
                    "empty_ring_returns_without_output": True,
                    "null_output_still_consumes_nonempty": True,
                    "output_physical_store": 0xd68c,
                    "translation_call": 0xd69c, "translation_status_checked": False,
                    "read_index_publication": 0xd6c8,
                    "invalid_index_rejected": False,
                    "status_return_contract_validated": False},
        "peek": {"entry": 0xd718, "ring_handle_offset": 0x250,
                 "null_ring_guard": False, "empty_ring_leaves_output": True,
                 "translation_call": 0xd774, "translation_status_checked": False,
                 "read_index_modified": False, "invalid_index_rejected": False,
                 "status_return_contract_validated": False},
        "release": {"entry": 0xd5a4, "ring_handle_offset": 0x254,
                    "null_ring_guard": False, "null_record_guard": False,
                    "record_word_offset": 4, "data_publication": 0xd608,
                    "index_publication": 0xd61c,
                    "invalid_index_response": "log and continue",
                    "full_ring_response": "log and continue",
                    "duplicate_release_guard": False,
                    "source_release_ack_or_completion_validated": False},
        "translation": {"entry": 0x1fdac, "map_handle_offset": 0x224,
                        "candidate": "u32(word(map+0x28) + physical - word(map+0x30))",
                        "inclusive_virtual_bounds_offsets": [0x18, 0x1c],
                        "first_candidate_store": 0x1fdcc,
                        "candidate_written_before_bounds_check": True,
                        "metadata_extent_checked": False, "alignment_checked": False,
                        "list_head_pointer_offset": 4, "node_next_offset": 0,
                        "linked_candidate_store": 0x1fe24,
                        "failure_status": 2, "success_status": 0,
                        "failure_preserves_last_unvalidated_candidate": True,
                        "native_list_step_bound_validated": False},
        "scope": {
            "complete_selected_body_pins": True,
            "conditional_metadata_translation": True,
            "public_PPB_layout_equivalence": False,
            "raw_source_lease": False, "runtime_context_identity": False,
            "all_consumer_completion": False, "generation_safe_reuse": False,
            "standalone_processing": False,
        },
        "conditions": [
            "A32 execution uses the pinned bodies and returning ABI-preserving log calls.",
            "Handle, output, ring and mapping storage is valid and disjoint unless an explicit counterexample supplies an alias.",
            "Ring/header/index words remain stable during each serialized call; concurrent producer/consumer visibility is not proved.",
            "The translated value identifies metadata only; neither that value nor a queue index establishes source extent or retention.",
        ],
    }


def _picture_output_map(payload, images):
    """Pure fixed A32 evidence; callers must pin the exact bundled SHA/size."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("picture-output payload size does not match the bundled baseline")
    identities = [(0x2ea60, 0x79dd8, 32, "little", 45, 2, 0x3a678),
                  (0x79dd8, 0xcfbb0, 32, "little", 45, 2, 0x49f68)]
    fields = ("blob_file_offset", "blob_file_end", "class", "endianness",
              "machine", "elf_type", "entry_virtual_address")
    if [tuple(i.get(field) for field in fields) for i in images] != identities:
        raise FormatError("picture-output ELF identities do not match the bundled baseline")

    # Only these independently decoded A32 sites are instructions. Neither the
    # whole prefix nor either embedded ARC image is treated as executable A32.
    groups = {
        "delivery": (
            (0x6ff4, 0xe92d4070), (0x6ff8, 0xe1a04001), (0x6ffc, 0xe59f5130),
            (0x7014, 0xe3140c02), (0x7018, 0x0a000002), (0x701c, 0xeb0001b9),
            (0x7020, 0xe3000200), (0x7024, 0xe5850008), (0x7708, 0xe92d4070),
            (0x7714, 0xebffe45f), (0x7720, 0xe51f1034), (0x7724, 0xe5911004),
            (0x7728, 0xe3002401), (0x772c, 0xe0811002), (0x7730, 0xe51f4088),
            (0x7734, 0xe5943024), (0x7768, 0xe3a02073), (0x776c, 0xe0020293),
            (0x7770, 0xe0804102), (0x7774, 0xe3a02020), (0x7778, 0xe2840f62),
            (0x777c, 0xeb009386), (0x7780, 0xe3a00001), (0x7784, 0xe5c401a8),
            (0x7788, 0xe8bd8070), (0x898, 0xe51f01a4), (0x89c, 0xe12fff1e),
        ),
        "pending_main": (
            (0x8e78, 0xe3a05000), (0x8e7c, 0xe1a00005), (0x8e80, 0xebfffa41),
            (0x8e84, 0xe3500000), (0x8e88, 0x0a000001), (0x8e8c, 0xe1a00005),
            (0x8e90, 0xebfffd2d), (0x8e94, 0xe2855001), (0x8e98, 0xe3550004),
            (0x8e9c, 0xbafffff6), (0x77ac, 0xe5d011a8), (0x77b0, 0xe3510000),
            (0x77b4, 0x0a000007), (0x77b8, 0xe5d000c4), (0x77bc, 0xe3500000),
            (0x77c0, 0x0a000004), (0x77c4, 0xe3a00001), (0x77c8, 0xe8bd8010),
            (0x77d8, 0xe3a00000), (0x77dc, 0xeafffff9), (0x77a0, 0xe3a01073),
            (0x77a4, 0xe0010194), (0x77a8, 0xe0800101), (0x778c, 0xe92d4010),
            (0x7790, 0xe1a04000), (0x7794, 0xebffe43f), (0x7798, 0xe3500000),
            (0x779c, 0x0a00000a),
        ),
        "picture_handler": (
            (0x834c, 0xe92d43f0), (0x8350, 0xe24dd0ac), (0x8394, 0xe5d400d2),
            (0x8398, 0xe3500001), (0x839c, 0x0a000005), (0x8354, 0xe1a09000),
            (0x8654, 0xe5d400c5), (0x8660, 0x0a000017), (0x866c, 0xe3100c01),
            (0x8670, 0x0a000033),
            (0x8698, 0xe1a01009), (0x869c, 0xe3a00001), (0x86a0, 0xebfffc7c),
            (0x8714, 0xe1a01009), (0x8718, 0xe3a00001), (0x871c, 0xebfffc5d),
            (0x8768, 0xebfffd19),
            (0x876c, 0xe51f0dc4), (0x8770, 0xe5901100), (0x8774, 0xe3811002),
            (0x8778, 0xe5801100),
            (0x87a8, 0xe1a01009), (0x87ac, 0xe3a00000), (0x87b0, 0xebfffc38),
            (0x8820, 0xe5c471a8), (0x8358, 0xe3a07000), (0x83b4, 0xe8bd83f0),
        ),
        "bop": (
            (0x7898, 0xe59f3188), (0x789c, 0xe5932000), (0x78a0, 0xe3500000),
            (0x78a4, 0x1a000003), (0x78a8, 0xe3820001), (0x78ac, 0xe5830000),
            (0x78b0, 0xe51f0220), (0x78b4, 0xe5801008), (0x78b8, 0x0a000001),
            (0x78bc, 0xe3c20001), (0x78c0, 0xe5830000), (0x78c4, 0xe59f1160),
            (0x78c8, 0xe3a00001), (0x78cc, 0xe5810030), (0x78d0, 0xe3a0002b),
            (0x78d4, 0xea000d8f),
        ),
        "dnr": (
            (0x82d0, 0xe92d4010), (0x82d4, 0xe3a03000), (0x82d8, 0xe51f28b4),
            (0x82dc, 0xe3a04001), (0x82e0, 0xe5824404), (0x82e4, 0xe3500e2d),
            (0x82e8, 0x9a000000), (0x82ec, 0xe3a03801), (0x82f0, 0xe5823408),
            (0x82f4, 0xe7df159f), (0x82f8, 0xe1810800), (0x82fc, 0xe582040c),
            (0x8348, 0xe8bd8010), (0x8610, 0xe1cd03d4), (0x8614, 0xebffff2d),
        ),
        "metadata_dma": (
            (0x82ac, 0xe51f0904), (0x82b0, 0xe580a114), (0x82b4, 0xe59d1014),
            (0x82b8, 0xe5801118), (0x82bc, 0xe5901100), (0x82c0, 0xe3811010),
            (0x82c4, 0xe5801100),
        ),
        "mfd": (
            (0x1bfc, 0xe92d41f0), (0x1c00, 0xe1a06000), (0x1c08, 0xe1a04003),
            (0x1c34, 0xe5945014), (0x1c3c, 0xe5960000), (0x1c40, 0xe5906004),
            (0x1c9c, 0xe1a02005), (0x1ca0, 0xe59f125c), (0x1ca4, 0xe1a00006),
            (0x1ca8, 0xeb00730e), (0x84cc, 0xe28d3020), (0x84d0, 0xe1a01009),
            (0x84d4, 0xe28d2014), (0x84d8, 0xe1a00008), (0x84dc, 0xebffe5c6),
            (0x1e8e8, 0xe5903000), (0x1e8ec, 0xe7832001), (0x1e8f0, 0xe12fff1e),
        ),
        "scl": (
            (0x1cf4, 0xe92d4070), (0x1cf8, 0xe1a06001), (0x1cfc, 0xe1a05002),
            (0x1d00, 0xe5900000), (0x1d04, 0xe5904004), (0x1d08, 0xe3a0200c),
            (0x1d0c, 0xe59f1224), (0x1d10, 0xe1a00004), (0x1d14, 0xeb0072f3),
            (0x1d38, 0xe1855806), (0x1d3c, 0xe59f1200), (0x1d40, 0xe1a02005),
            (0x1d44, 0xe1a00004), (0x1d48, 0xeb0072e6), (0x1d6c, 0xe59f11dc),
            (0x1d70, 0xe1a02005), (0x1d74, 0xe1a00004), (0x1d78, 0xeb0072da),
            (0x1e4c, 0xe1a00004), (0x1e50, 0xe59f1130), (0x1e54, 0xe8bd4070),
            (0x1e58, 0xe3a02001), (0x1e5c, 0xea0072a1), (0x2034, 0xe5941014),
            (0x2038, 0xe1a00008), (0x203c, 0xe5942018), (0x2040, 0xebffff2b),
        ),
        "key_stubs": (
            (0x3ed8, 0xe92d4070), (0x3edc, 0xe3500000), (0x3ee0, 0x0a000009),
            (0x3ee4, 0xe2805014), (0x3ee8, 0xe2804f45), (0x3eec, 0xe28f0f7b),
            (0x3ef0, 0xeb007133), (0x3ef4, 0xe3a00000), (0x3ef8, 0xe5840008),
            (0x3efc, 0xe5950004), (0x3f00, 0xe5840004), (0x3f04, 0xe3a00000),
            (0x3f08, 0xe8bd8070), (0x3f0c, 0xe1a01000), (0x3f10, 0xe59f01fc),
            (0x3f14, 0xeb00712a), (0x3f18, 0xe3a00002), (0x3f1c, 0xeafffff9),
            (0x3f20, 0xe92d4070), (0x3f24, 0xe3500000), (0x3f28, 0x0a000009),
            (0x3f2c, 0xe2805014), (0x3f30, 0xe2804f45), (0x3f34, 0xe28f0f77),
            (0x3f38, 0xeb007121), (0x3f3c, 0xe3a00000), (0x3f40, 0xe5840008),
            (0x3f44, 0xe5950004), (0x3f48, 0xe5840004), (0x3f4c, 0xe3a00000),
            (0x3f50, 0xe8bd8070), (0x3f54, 0xe1a01000), (0x3f58, 0xe59f01ec),
            (0x3f5c, 0xeb007118), (0x3f60, 0xe3a00002), (0x3f64, 0xeafffff9),
        ),
        "key_callers": ((0x6908, 0xebfff584), (0x6928, 0xebfff56a)),
    }
    literals = {
        0x6ffc: (0x7134, 0x100f2000, 5), 0x7720: (0x76f4, 0x100f6000, 1),
        0x7730: (0x76b0, 0x100e0000, 4), 0x898: (0x6fc, 0xd3a00, 0),
        0x7898: (0x7a28, 0x10510000, 3), 0x78b0: (0x7698, 0xd2210, 0),
        0x78c4: (0x7a2c, 0x10540000, 1), 0x82d8: (0x7a2c, 0x10540000, 2),
        0x82ac: (0x79b0, 0x10502000, 0), 0x876c: (0x79b0, 0x10502000, 0),
        0x1ca0: (0x1f04, 0x00540014, 1), 0x1d0c: (0x1f38, 0x00540804, 1),
        0x1d3c: (0x1f44, 0x00540810, 1), 0x1d6c: (0x1f50, 0x0054081c, 1),
        0x1e50: (0x1f88, 0x00540854, 1), 0x3f10: (0x4114, 0x2cf84, 0),
        0x3f58: (0x414c, 0x2cfdc, 0),
    }
    anchors = []
    for group, sites in groups.items():
        for offset, expected in sites:
            if len(anchors) >= MAX_PICTURE_OUTPUT_ANCHORS:
                raise FormatError("picture-output instruction-anchor budget exceeded")
            actual = _bootstrap_word(payload, offset)
            if actual != expected:
                raise FormatError(f"picture-output word at {offset:#x} does not match the baseline")
            record = {"blob_file_offset": offset, "word": actual,
                      "operation": "validated word", "group": group}
            if offset in literals:
                record.update(_a32_literal(payload, offset))
                if (record["literal_blob_file_offset"], record["literal_value"],
                        record["destination_register"]) != literals[offset]:
                    raise FormatError("picture-output literal does not match the baseline")
            elif actual & 0x0e000000 == 0x0a000000:
                # Decode a branch only after matching its fixed audited word.
                # This includes the one fixed BLS at 0x82e8, not a general scan.
                displacement = actual & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                target = offset + 8 + displacement * 4
                _bootstrap_word(payload, target)
                record.update(operation="BL" if actual & (1 << 24) else "B",
                              condition=actual >> 28, target_blob_file_offset=target)
            anchors.append(record)

    diagnostics = []
    for offset, expected in (
            (0x40e0, b"[fw] SMP_CmdIf_SetSessionKey(): NOT Implemented\n"),
            (0x4118, b"[fw] SMP_CmdIf_SetContentKey(): NOT Implemented\n"),
            (0x2cf84, b"[fw] SMP_CmdIf_SetSessionKey(): Invalid Parameter with Command Header Address = 0x%x\n"),
            (0x2cfdc, b"[fw] SMP_CmdIf_SetContentKey(): Invalid Parameter with Command Header Address = 0x%x\n")):
        if bounded(payload, offset, len(expected) + 1, "picture-output diagnostic") != expected + b"\0":
            raise FormatError("picture-output diagnostic does not match the baseline")
        diagnostics.append({"blob_file_offset": offset, "text": expected.decode("ascii")})
    # ADR r0,PC+imm for the two non-null diagnostics; rotated-immediate A32.
    for offset, target in ((0x3eec, 0x40e0), (0x3f34, 0x4118)):
        instruction = _bootstrap_word(payload, offset)
        rotation = ((instruction >> 8) & 15) * 2
        value = instruction & 255
        immediate = ((value >> rotation) | (value << ((32 - rotation) % 32))) & 0xffffffff
        if offset + 8 + immediate != target:
            raise FormatError("picture-output diagnostic ADR does not match the baseline")

    rdb = "include/flea/70015/magnum/basemodules/chp/70015/rdb/a0/"
    # Context-relative writes do not establish that context's physical base.
    # Direct ARM physical addresses and RDB offsets are separate namespaces.
    operations = [
        ("BOP_AES_CTRL", 0x10510000, 0x00510000, [0x78ac, 0x78c0], 1,
         "START_ENCRYPTION_SCRAMBLE: set when r0==0, clear otherwise", "bchp_bop_aes.h:97"),
        ("MISC2_GLOBAL_CTRL", 0x10502100, 0x00502100, [0x82c4, 0x8778], 0x12,
         "META_DMA_ENABLE set at 0x82bc..0x82c4; BVN_YUY2_MODE set at 0x8770..0x8778",
         "bchp_misc2.h:80"),
        ("MFD_PIC_FEED_CMD", 0x10540030, 0x00540030, [0x78cc], 1,
         "START_FEED written as 1", "bchp_mfd.h:323"),
        ("MFD_DISP_HSIZE", None, 0x00540014, [0x1ca8], 0x1fff,
         "descriptor word at offset 0x14 passed to register-write helper", "bchp_mfd.h:232"),
        ("DNR_DNR_TOP_CTRL", 0x10540404, 0x00540404, [0x82e0], 1,
         "DNR_ENABLE written as 1", "bchp_dnr.h:108"),
        ("DNR_LINE_STORE_CONFIG", 0x10540408, 0x00540408, [0x82f0], 0x10000,
         "LS_MODE HD bit set only when input width > 720", "bchp_dnr.h:121"),
        ("DNR_SRC_PIC_SIZE", 0x1054040c, 0x0054040c, [0x82fc], 0x07ff07ff,
         "width<<16 | (height & 0x7ff); no width masking is established", "bchp_dnr.h:138"),
        ("SCL_HD_TOP_CONTROL", None, 0x00540804, [0x1d14], 12,
         "ENABLE_CTRL and UPDATE_SEL picture-controlled bits written as 12", "bchp_scl_hd.h:299"),
        ("SCL_HD_BVB_IN_SIZE", None, 0x00540810, [0x1d48], 0x07ff07ff,
         "width<<16 | height passed to register-write helper", "bchp_scl_hd.h:410"),
        ("SCL_HD_DEST_PIC_SIZE", None, 0x0054081c, [0x1d78], 0x07ff07ff,
         "same packed value as BVB_IN_SIZE in this selected path", "bchp_scl_hd.h:467"),
        ("SCL_HD_ENABLE", None, 0x00540854, [0x1e5c], 1,
         "enable value 1 passed to register-write helper", "bchp_scl_hd.h:657"),
    ]
    return {
        "schema_version": 1, "isa": "A32", "endianness": "little", "device_observed": False,
        "instruction_anchors": anchors, "diagnostics": diagnostics,
        "mfd_source": _mfd_source_map(payload),
        "descriptor_delivery": {
            "reader_entry_blob_file_offset": 0x7708, "arm2_callback_call_blob_file_offset": 0x701c,
            "arm_mailbox_physical_address": 0x100e0024, "arm_mailbox_rdb_address": 0x000e0024,
            "source_expression": "BORCH_END + 0x401", "slot_stride_bytes": 0x1cc,
            "destination_slot_offset": 0x188, "copy_argument_bytes": 32,
            "copy_helper_entry_blob_file_offset": 0x2c59c, "copy_helper_body_validated": False,
            "pending_slot_offset": 0x1a8, "active_slot_offset": 0xc4,
            "main_slot_range": [0, 3], "main_call_blob_file_offset": 0x8e90,
            "picture_handler_entry_blob_file_offset": 0x834c,
            "started_slot_offset": 0xd2, "pending_clear_blob_file_offset": 0x8820,
            "host_record_source": "include/flea/DriverFwShare.h:22",
            "host_submit_source": "driver/linux/crystalhd_fleafuncs.c:2255",
            "complete_dma_ownership_verified": False},
        "picture_feed": {
            "entry_blob_file_offset": 0x7898,
            "callers": [{"blob_file_offset": offset, "r0": value, "r1": "slot"}
                        for offset, value in ((0x86a0, 1), (0x871c, 1), (0x87b0, 0))],
            "scope": "Picture-feed trigger and output encryption/scramble control, not generic AES or input decryption."},
        "packing_override": {
            "picture_handler_entry_blob_file_offset": 0x834c,
            "base_load_blob_file_offset": 0x876c,
            "base_literal_blob_file_offset": 0x79b0,
            "arm_base_address": 0x10502000,
            "register_byte_offset": 0x100,
            "arm_physical_address": 0x10502100,
            "rdb_address": 0x00502100,
            "read_blob_file_offset": 0x8770,
            "set_blob_file_offset": 0x8774,
            "write_blob_file_offset": 0x8778,
            "set_mask": 0x2,
            "field": "BVN_YUY2_MODE",
            "preceding_metadata_setup": {
                "entry_blob_file_offset": 0x7bd4,
                "caller_blob_file_offset": 0x8768,
                "base_load_blob_file_offset": 0x82ac,
                "metadata_base_write_blob_file_offset": 0x82b0,
                "metadata_length_write_blob_file_offset": 0x82b8,
                "read_blob_file_offset": 0x82bc,
                "set_blob_file_offset": 0x82c0,
                "write_blob_file_offset": 0x82c4,
                "set_mask": 0x10,
                "field": "META_DMA_ENABLE"},
            "combined_firmware_set_mask": 0x12,
            "selected_path_predicates": {
                "slot_state_byte_offset": 0xc5,
                "slot_state_required": "nonzero",
                "slot_state_branch_blob_file_offset": 0x8660,
                "picture_word_mask": 0x100,
                "picture_word_required": "clear",
                "picture_word_branch_blob_file_offset": 0x8670},
            "selected_path": "slot state byte 0xc5 nonzero and picture word bit 0x100 clear before the picture-feed call at 0x87b0",
            "overwrite_path_established": True,
            "exclusive_register_writer_proved": False,
            "persistent_hardware_uyvy_across_picture_delivery": False,
            "scope": "Stock firmware repeats a MISC2_GLOBAL_CTRL read-modify-write that forces its YUY2 packing bit; generic indirect writers and the origin of preserved bits are outside this proof."},
        "register_operations": [
            {"name": name, "arm_physical_address": physical, "rdb_address": address,
             "instruction_blob_file_offsets": sites, "field_mask": mask,
             "selected_operation": operation, "source": rdb + source,
             "register_write_helper_entry_blob_file_offset": 0x1e8e8 if physical is None else None}
            for name, physical, address, sites, mask, operation, source in operations],
        "key_handler_stubs": [
            {"name": name, "entry_blob_file_offset": entry, "last_instruction_blob_file_offset": end,
             "caller_blob_file_offset": caller, "diagnostic_blob_file_offset": diagnostic,
             "nonnull_reply_status": 0, "request_record_offset": 0x14, "reply_record_offset": 0x114,
             "sequence_word_index": 1, "status_word_index": 2,
             "key_payload_reads_in_bounded_body": False, "key_provisioning_verified": False,
             "scope": "Non-null record ACK stub with NOT Implemented diagnostic; status zero is not key provisioning."}
            for name, entry, end, caller, diagnostic in (
                ("SetSessionKey", 0x3ed8, 0x3f1c, 0x6928, 0x40e0),
                ("SetContentKey", 0x3f20, 0x3f64, 0x6908, 0x4118))],
        "deferred": ["CSC dispatcher routing and coefficient programming are not validated here.",
                     "Full scaler/filter configuration and arbitrary raw-frame operations are not validated here."],
        "limitations": ["Only fixed A32 anchors and bounded diagnostic data are validated, not a complete call graph.",
                        "Descriptor submission is not complete DMA ownership, lifetime or completion proof.",
                        "ARM physical address/RDB correspondence does not establish host GISB access, clock/reset readiness or safe reads.",
                        "Context-relative register writes do not establish their physical base.",
                        "The log routine and copy-helper implementations, ARC paths and Thumb helpers are not decoded.",
                        "No hardware execution, key provisioning, cipher transformation or standalone processing capability is verified."]}


def string_at(table, offset):
    if offset < 0 or offset >= len(table):
        raise FormatError("string index is outside its ELF string table")
    # Keep malformed string tables bounded even with many distinct indexes.
    end = table.find(b"\0", offset, min(len(table), offset + 4096))
    if end < 0:
        raise FormatError("unterminated or oversized ELF string")
    return table[offset:end].decode("ascii", errors="backslashreplace")


def function_intervals(symbol_tables, sections):
    """Index sized functions by section, without collapsing aliases or tables."""
    by_section = {}
    for symbols in symbol_tables.values():
        for symbol in symbols:
            index = symbol["section_index"]
            if (symbol["type"] == 2 and symbol["size"] and 0 < index < len(sections)
                    and sections[index]["flags"] & 4):
                by_section.setdefault(index, []).append(symbol)
    result = {}
    for index, symbols in by_section.items():
        symbols.sort(key=lambda symbol: (symbol["elf_virtual_address"],
                                         symbol["symbol_table_section_index"],
                                         symbol["symbol_index"]))
        starts = []
        prefix_ends = []
        end = 0
        for symbol in symbols:
            start = symbol["elf_virtual_address"]
            end = max(end, start + symbol["size"])
            starts.append(start)
            prefix_ends.append(end)
        result[index] = (symbols, starts, prefix_ends)
    return result


def parse_references(image, base, sections, symbol_tables, wanted, all_symbols,
                     relocation_budget, owner_budget, output_budget):
    # ET_EXEC r_offset is a VA in sh_info's section, not a file offset. Keep
    # numeric types: old ARC ABI revisions disagree on their names/semantics.
    # https://gabi.xinuos.com/elf/06-reloc.html
    tables = [(index, section) for index, section in enumerate(sections)
              if section["type"] in (4, 9)]  # SHT_RELA / SHT_REL
    if any(table["type"] == 9 for _, table in tables):
        raise FormatError("SHT_REL references without explicit addends are unsupported")
    count = sum(table["size"] // 12 for _, table in tables)
    if count > relocation_budget:
        raise FormatError("ELF relocation-record budget exceeded")
    intervals = function_intervals(symbol_tables, sections)
    references = []
    type_counts = {}
    noop_count = unowned_count = owner_steps = output_bytes = 0
    encoded_symbol_sizes = {}
    for table_index, table in tables:
        if table["entry_size"] != 12 or table["size"] % 12:
            raise FormatError("invalid ELF32 RELA table size")
        if table["link"] not in symbol_tables:
            raise FormatError("ELF relocations do not link to a symbol table")
        if not 0 < table["info"] < len(sections) or sections[table["info"]]["type"] == 0:
            raise FormatError("ELF relocation target section index is invalid")
        source_section = sections[table["info"]]
        symbols = symbol_tables[table["link"]]
        for offset in range(table["offset"], table["offset"] + table["size"], 12):
            address, info, addend = struct.unpack_from("<IIi", image, offset)
            symbol_index, kind = info >> 8, info & 0xff
            if symbol_index >= len(symbols):
                raise FormatError("ELF relocation symbol index is outside its symbol table")
            type_counts[str(kind)] = type_counts.get(str(kind), 0) + 1
            # The blob retains no-ops at the exclusive end of .text. They
            # represent no source byte and cannot be used to infer an edge.
            if kind == 0:
                noop_count += 1
                continue
            delta = address - source_section["address"]
            if not 0 <= delta < source_section["size"]:
                raise FormatError("ELF relocation source extends outside its target section")
            source_file_offset = (None if source_section["type"] == 8 else
                                  base + source_section["offset"] + delta)
            owners = []
            if table["info"] in intervals:
                functions, starts, prefix_ends = intervals[table["info"]]
                index = bisect_right(starts, address) - 1
                while index >= 0 and prefix_ends[index] > address:
                    if owner_steps >= owner_budget:
                        raise FormatError("ELF function-owner lookup budget exceeded")
                    owner_steps += 1
                    function = functions[index]
                    if address < function["elf_virtual_address"] + function["size"]:
                        owners.append(function)
                    index -= 1
                owners.sort(key=lambda symbol: (symbol["symbol_table_section_index"],
                                                symbol["symbol_index"]))
            if not owners and source_section["flags"] & 4:
                unowned_count += 1
            target = symbols[symbol_index]
            if not (all_symbols or target["name"] in wanted or
                    any(owner["name"] in wanted for owner in owners)):
                continue
            # S+A is only an arithmetic candidate, NOT an applied relocation.
            # Never resolve via a global VA search: overlay sections share VAs.
            candidate = target["elf_virtual_address"] + addend
            candidate_in_section = False
            candidate_file_offset = None
            target_index = target["section_index"]
            if 0 < target_index < len(sections) and 0 <= candidate < 1 << 32:
                target_section = sections[target_index]
                candidate_delta = candidate - target_section["address"]
                candidate_in_section = 0 <= candidate_delta < target_section["size"]
                if (candidate_in_section and target_section["type"] not in (0, 8)
                        and target_section["flags"] & 2):
                    candidate_file_offset = base + target_section["offset"] + candidate_delta
            # Repeated long names/aliases must not expand a small input into
            # unbounded JSON. Cache symbol accounting, not per-use output.
            cost = 1024 + 6 * len(source_section["name"])
            for symbol in [target] + owners:
                identity = (symbol["symbol_table_section_index"], symbol["symbol_index"])
                if identity not in encoded_symbol_sizes:
                    encoded_symbol_sizes[identity] = len(json.dumps(symbol, ensure_ascii=True)) + 768
                cost += encoded_symbol_sizes[identity]
            if output_bytes + cost > output_budget:
                raise FormatError("ELF reference-output byte budget exceeded")
            output_bytes += cost
            references.append({"relocation_record_offset": base + offset,
                               "relocation_section_index": table_index,
                               "relocation_type": kind, "addend": addend,
                               "source": {"section_index": table["info"],
                                          "section": source_section["name"],
                                          "elf_virtual_address": address,
                                          "blob_file_offset": source_file_offset,
                                          "function_owners": owners},
                               "target": {"symbol": target,
                                          "addend_candidate_virtual_address": candidate,
                                          "addend_candidate_blob_file_offset": candidate_file_offset,
                                          "addend_candidate_in_section": candidate_in_section}})
    return {"relocation_count": count, "relocation_type_counts": type_counts,
            "relocation_noop_count": noop_count,
            "unowned_executable_reference_count": unowned_count,
            "owner_lookup_steps": owner_steps, "reference_output_budget_used": output_bytes,
            "references": references}


def parse_elf(payload, base, wanted, symbol_budget, string_budget,
              references=False, all_symbols=False, relocation_budget=0,
              owner_budget=0, output_budget=0, metadata_budget=MAX_METADATA_OUTPUT_BYTES,
              arc_metadata=False):
    # ELF32 Ehdr/Phdr/Shdr/Sym layouts follow https://gabi.xinuos.com/elf/.
    image = memoryview(payload)[base:]
    header = bounded(image, 0, 52, "ELF header")
    if bytes(header[:7]) != b"\x7fELF\x01\x01\x01":
        raise FormatError("only ELF32 little-endian version 1 is supported")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", header, 16)
    kind, machine, version, entry, phoff, shoff, flags = fields[:7]
    ehsize, phsize, phcount, shsize, shcount, names_index = fields[7:]
    if kind != 2 or machine != 45 or version != 1:
        raise FormatError("embedded image is not an ELF32 ARC executable")
    if ehsize != 52 or phsize != 32 or shsize != 40:
        raise FormatError("unsupported ELF header entry size")
    if (not phcount or phcount == 0xffff or not 0 < shcount < 0xff00
            or names_index >= shcount):
        raise FormatError("missing or unsupported extended ELF header table")
    bounded(image, phoff, phsize * phcount, "ELF program table")
    bounded(image, shoff, shsize * shcount, "ELF section table")
    extent = max(52, phoff + phsize * phcount, shoff + shsize * shcount)
    segments = []
    for index in range(phcount):
        ptype, offset, address, physical, filesz, memsz, pflags, align = (
            struct.unpack_from("<8I", image, phoff + index * phsize))
        if ptype == 1 and filesz > memsz:
            raise FormatError("ELF load segment has more file bytes than memory bytes")
        if ptype == 1 and address + memsz > 1 << 32:
            raise FormatError("ELF load segment overflows its 32-bit virtual address range")
        if filesz:
            bounded(image, offset, filesz, "ELF segment")
            extent = max(extent, offset + filesz)
        if ptype == 1:
            segments.append({"elf_virtual_address": address,
                             "blob_file_offset": base + offset if filesz else None,
                             "file_size": filesz, "memory_size": memsz,
                             "flags": pflags})

    sections = []
    for index in range(shcount):
        fields = struct.unpack_from("<10I", image, shoff + index * shsize)
        section = dict(zip(("name_index", "type", "flags", "address", "offset",
                            "size", "link", "info", "align", "entry_size"), fields))
        if section["flags"] & 2 and section["address"] + section["size"] > 1 << 32:
            raise FormatError("ELF section overflows its 32-bit virtual address range")
        # SHT_NULL and SHT_NOBITS do not occupy file-backed bytes.
        if section["type"] not in (0, 8) and section["size"]:
            bounded(image, section["offset"], section["size"], "ELF section")
            extent = max(extent, section["offset"] + section["size"])
        sections.append(section)
    names = sections[names_index]
    if names["type"] != 3:
        raise FormatError("ELF section names do not link to a string table")
    if names["size"] > string_budget:
        raise FormatError("ELF string-table byte budget exceeded")
    name_table = bytes(bounded(image, names["offset"], names["size"], "section names"))
    string_table_bytes = names["size"]
    string_cache = {(names["offset"], names["size"]): name_table}
    metadata_bytes = 0
    for section in sections:
        name = string_at(name_table, section["name_index"])
        # Bound decoded names before retaining them, including repeated names
        # and metadata-only --all-symbols runs that never parse references.
        metadata_bytes += 1024 + 6 * len(name)
        if metadata_bytes > metadata_budget:
            raise FormatError("ELF retained-metadata byte budget exceeded")
        section["name"] = name

    symbols = []
    symbol_count = 0
    # SHT_SYMTAB is retained linker metadata, not merely matching strings.
    symbol_tables = [(index, table) for index, table in enumerate(sections) if table["type"] == 2]
    if sum(table["size"] // 16 for _, table in symbol_tables) > symbol_budget:
        raise FormatError("ELF symbol-record budget exceeded")
    indexed_symbols = {}
    for table_index, table in symbol_tables:
        if references:
            indexed_symbols[table_index] = []
        if table["entry_size"] != 16 or table["size"] % 16:
            raise FormatError("invalid ELF32 symbol table size")
        if table["link"] >= shcount or sections[table["link"]]["type"] != 3:
            raise FormatError("ELF symbols do not link to a string table")
        if not table["size"]:
            continue
        strings = sections[table["link"]]
        key = (strings["offset"], strings["size"])
        if key not in string_cache:
            if string_table_bytes + strings["size"] > string_budget:
                raise FormatError("ELF string-table byte budget exceeded")
            string_cache[key] = bytes(bounded(image, *key, "symbol names"))
            string_table_bytes += strings["size"]
        string_table = string_cache[key]
        for offset in range(table["offset"], table["offset"] + table["size"], 16):
            name_index, value, size, info, other, section_index = (
                struct.unpack_from("<IIIBBH", image, offset))
            symbol_count += 1
            name = string_at(string_table, name_index)
            file_offset = None
            section_name = None
            if 0 < section_index < shcount:
                section = sections[section_index]
                section_name = section["name"]
                delta = value - section["address"]
                if delta < 0 or delta > section["size"] - size:
                    raise FormatError(f"symbol {name} extends outside its section")
                if (section["type"] not in (0, 8) and section["flags"] & 2
                        and delta < section["size"]):
                    file_offset = base + section["offset"] + delta
            elif section_index < 0xff00 and section_index != 0:
                raise FormatError("symbol section index is outside the ELF section table")
            elif section_index == 0xffff:
                raise FormatError("extended symbol section indexes are unsupported")
            if not (references or all_symbols or name in wanted):
                continue
            metadata_bytes += 1024 + 6 * (len(name) + len(section_name or ""))
            if metadata_bytes > metadata_budget:
                raise FormatError("ELF retained-metadata byte budget exceeded")
            symbol = {"name": name, "elf_virtual_address": value,
                      "blob_file_offset": file_offset, "size": size,
                      "type": info & 15, "binding": info >> 4,
                      "visibility": other & 3, "section": section_name,
                      "section_index": section_index,
                      "symbol_record_offset": base + offset}
            if references or all_symbols:
                symbol["symbol_table_section_index"] = table_index
                symbol["symbol_index"] = (offset - table["offset"]) // 16
            if references:
                indexed_symbols[table_index].append(symbol)
            if all_symbols or name in wanted:
                symbols.append(symbol)

    role_hints = [section["name"] for section in sections
                  if "outerloop" in section["name"] or "innerloop" in section["name"]]
    result = {"blob_file_offset": base, "blob_file_end": base + extent,
            "class": 32, "endianness": "little", "machine": machine,
            "elf_type": kind, "flags": flags, "entry_virtual_address": entry,
            "program_header_count": phcount, "section_count": shcount,
            "symbol_count": symbol_count, "string_table_bytes": string_table_bytes,
            "load_segments": segments,
            "role_hint_sections": role_hints,
            "symbols": symbols,
            "missing_symbols": sorted(wanted - {symbol["name"] for symbol in symbols}),
            "_metadata_budget_used": metadata_bytes}
    if all_symbols or arc_metadata:
        section_metadata = [{"section_index": index, "name": section["name"],
                               "type": section["type"], "flags": section["flags"],
                               "elf_virtual_address": section["address"],
                               "blob_file_offset": (base + section["offset"]
                                                    if section["type"] not in (0, 8)
                                                    and section["size"] else None),
                               "size": section["size"], "link": section["link"],
                               "info": section["info"], "align": section["align"],
                               "entry_size": section["entry_size"],
                               "section_header_blob_file_offset": base + shoff + index * shsize}
                              for index, section in enumerate(sections)]
        if all_symbols:
            result["sections"] = section_metadata
        if arc_metadata:
            result["_arc_sections"] = [s for s in section_metadata if s["name"] in
                                       (".comment", ".arcextmap", ".ARC.attributes")]
    if references:
        result.update(parse_references(image, base, sections, indexed_symbols, wanted,
                                       all_symbols, relocation_budget, owner_budget, output_budget))
    return result


def analyze(data, wanted=DEFAULT_SYMBOLS, expected_sha256=BUNDLED_SHA256,
            references=False, all_symbols=False, bootstrap=False, picture_output=False,
            arc_metadata=False, csc_command=False, command_buffer_bridge=False, inner_descriptor=False,
            scaler_fir=False, ppb_handoff=False, ppb_source=False, ppb_saved_context=False, ppb_stop_context=False,
            ppb_fixed_metadata=False, ppb_return_header=False, ppb_bank_ledger=None,
            debug_mechanisms=False, rx_descriptor_admission=False, channel_fields=False):
    if len(data) < 24 or len(data) > MAX_FIRMWARE_SIZE or len(data) % 4:
        raise FormatError("invalid BCM70015 firmware size")
    sha256 = hashlib.sha256(data).hexdigest()
    if sha256 != expected_sha256:
        raise FormatError("firmware SHA-256 does not match --expect-sha256")
    if bootstrap and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--bootstrap requires the exact bundled firmware SHA-256 and size")
    if picture_output and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--picture-output requires the exact bundled firmware SHA-256 and size")
    if arc_metadata and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--arc-metadata requires the exact bundled firmware SHA-256 and size")
    if csc_command and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--csc-command requires the exact bundled firmware SHA-256 and size")
    if command_buffer_bridge and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--command-buffer-bridge requires the exact bundled firmware SHA-256 and size")
    if inner_descriptor and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--inner-descriptor requires the exact bundled firmware SHA-256 and size")
    if scaler_fir and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--scaler-fir requires the exact bundled firmware SHA-256 and size")
    if ppb_handoff and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-handoff requires the exact bundled firmware SHA-256 and size")
    if ppb_source and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-source requires the exact bundled firmware SHA-256 and size")
    if ppb_saved_context and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-saved-context requires the exact bundled firmware SHA-256 and size")
    if ppb_stop_context and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-stop-context requires the exact bundled firmware SHA-256 and size")
    if ppb_fixed_metadata and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-fixed-metadata requires the exact bundled firmware SHA-256 and size")
    if ppb_return_header and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-return-header requires the exact bundled firmware SHA-256 and size")
    if ppb_bank_ledger is not None and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--ppb-bank-ledger requires the exact bundled firmware SHA-256 and size")
    if debug_mechanisms and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--debug-mechanisms requires the exact bundled firmware SHA-256 and size")
    if rx_descriptor_admission and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--rx-descriptor-admission requires the exact bundled firmware SHA-256 and size")
    if channel_fields and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--channel-fields requires the exact bundled firmware SHA-256 and size")
    payload = data[:-TRAILER_SIZE]
    length_slot = struct.unpack_from("<I", data, len(payload))[0]
    if length_slot != 16:
        raise FormatError("firmware trailer does not match the BCM70015 signature layout")
    images = []
    arc_sections = []
    symbol_budget = MAX_SYMBOL_RECORDS
    string_budget = MAX_STRING_TABLE_BYTES
    relocation_budget = MAX_RELOCATION_RECORDS
    owner_budget = MAX_OWNER_LOOKUP_STEPS
    output_budget = MAX_REFERENCE_OUTPUT_BYTES
    metadata_budget = MAX_METADATA_OUTPUT_BYTES
    wanted = set(wanted)
    if debug_mechanisms:
        wanted.update(_DEBUG_MECHANISM_SYMBOLS)
    offset = payload.find(b"\x7fELF")
    while offset >= 0:
        if len(images) == 16:
            raise FormatError("too many embedded ELF images")
        image = parse_elf(payload, offset, wanted, symbol_budget, string_budget,
                          references, all_symbols, relocation_budget, owner_budget,
                          output_budget, metadata_budget, arc_metadata)
        if arc_metadata:
            arc_sections.append(image.pop("_arc_sections"))
        symbol_budget -= image["symbol_count"]
        string_budget -= image["string_table_bytes"]
        metadata_budget -= image.pop("_metadata_budget_used")
        if references:
            relocation_budget -= image["relocation_count"]
            owner_budget -= image["owner_lookup_steps"]
            output_budget -= image["reference_output_budget_used"]
        if images and offset < images[-1]["blob_file_end"]:
            raise FormatError("embedded ELF file extents overlap")
        images.append(image)
        offset = payload.find(b"\x7fELF", offset + 4)
    if not images:
        raise FormatError("no embedded ELF32 ARC executables found")
    vectors = []
    for offset in (0, 4, 8, 12, 16, 24, 28):
        instruction = struct.unpack_from("<I", payload, offset)[0]
        if instruction & 0xfffff000 == 0xe59ff000:
            literal = offset + 8 + (instruction & 0xfff)
            target = struct.unpack("<I", bounded(payload, literal, 4, "ARM vector literal"))[0]
            vectors.append({"blob_file_offset": offset,
                            "literal_blob_file_offset": literal, "target_value": target})
    revisions = [{"blob_file_offset": match.start(),
                  "text": match.group().decode("ascii")}
                 for match in re.finditer(rb"\$Media_PC_FW_Rev: [0-9.]+ \$", payload)]
    result = {"schema_version": 1, "sha256": sha256,
            "git_blob_sha1": hashlib.sha1(b"blob " + str(len(data)).encode("ascii")
                                         + b"\0" + data).hexdigest(),
            "bundled_baseline": sha256 == BUNDLED_SHA256, "size": len(data),
            "payload_end": len(payload), "trailer_length_slot": length_slot,
            "signature_file_offset": len(data) - 16,
            "signature_verified": False, "firmware_revisions": revisions,
            "arm_vector_candidates": vectors, "images": images,
            "limitations": ["ELF virtual addresses are not host/device DRAM addresses.",
                            "Role hints do not identify an exact ARC core or usable codecs.",
                            "Symbols do not prove UART or host-mailbox accessibility.",
                            "ARM image extent and command call graph are not established."]}
    if references:
        result["limitations"].extend([
            "Retained references are incomplete and are not a proven instruction call graph.",
            "Relocation types are numeric; their ARC encoding semantics are not applied.",
            "S+A arithmetic candidates are not resolved targets; file mappings require containment in the referenced section.",
            "No-op relocations have no reference edge; unowned sites are not assigned to nearby functions."])
    if bootstrap:
        result["bootstrap"] = _bootstrap_map(payload, images)
    if picture_output:
        result["picture_output"] = _picture_output_map(payload, images)
    if arc_metadata:
        result["arc_metadata"] = _arc_metadata_map(payload, images, arc_sections)
    if csc_command:
        result["csc_command"] = _csc_command_map(payload, images)
    if command_buffer_bridge:
        result["command_buffer_bridge"] = _command_buffer_bridge_map(payload, images)
    if inner_descriptor:
        result["inner_descriptor"] = _inner_descriptor_map(payload, images)
        # Enrich only the explicit public option. Other proofs use the original
        # private map's conservative preflight union and remain unchanged.
        pointer_path = result["inner_descriptor"]["paths"]["record_pointer_and_boundary"]
        pointer_path["conditional_inner_dispatch"] = _inner_dispatch_map(payload)
    if scaler_fir:
        result["scaler_fir"] = _scaler_fir_map(payload)
    if ppb_handoff:
        result["arm_ppb_metadata_handoff"] = _arm_ppb_metadata_handoff(payload)
    if ppb_source:
        result["ppb_source_provenance"] = _ppb_source_provenance(payload)
    if ppb_saved_context:
        result["ppb_saved_context_bridge"] = _ppb_saved_context_bridge(payload)
    if ppb_stop_context:
        result["ppb_stop_context_bridge"] = _ppb_stop_context_bridge(payload)
    if ppb_fixed_metadata:
        result["ppb_fixed_metadata_bridge"] = _ppb_fixed_metadata_bridge(payload)
    if ppb_return_header:
        result["ppb_return_header_bridge"] = _ppb_return_header_bridge(payload)
    if ppb_bank_ledger is not None:
        result["ppb_bank_ledger"] = _ppb_bank_decode_ledger(payload, ppb_bank_ledger)
    if debug_mechanisms:
        result["debug_mechanisms"] = _debug_mechanism_map(payload, images)
    if rx_descriptor_admission:
        result["rx_descriptor_admission"] = _rx_descriptor_admission_map(payload)
    if channel_fields:
        result["channel_fields"] = _channel_field_map(payload)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Linux only; reads a regular firmware file and emits JSON to stdout. "
        "No device ioctls, firmware execution, extraction or patching. "
        "Supply an ordinary offline file, not a sysfs/debugfs attribute. "
        "The expected SHA-256 defaults to the bundled BCM70015 blob; "
        "all offsets refer to that exact input file."))
    parser.add_argument("firmware", help="regular firmware file (not a symlink)")
    parser.add_argument("--expect-sha256", default=BUNDLED_SHA256,
                        help="explicitly select another SHA-256-pinned blob")
    parser.add_argument("--symbol", action="append", help="exact ELF symbol name; repeatable")
    parser.add_argument("--references", action="store_true", help=(
        "inventory retained RELA references whose target or sized source function matches --symbol; "
        "not disassembly or a complete call graph"))
    parser.add_argument("--all-symbols", action="store_true", help=(
        "include all symbol records and section metadata; with --references include all retained references"))
    parser.add_argument("--bootstrap", action="store_true", help=(
        "validate fixed ARM bootstrap/mailbox anchors and image catalog; bundled firmware only"))
    parser.add_argument("--picture-output", action="store_true", help=(
        "validate fixed picture-output and key ACK-stub anchors; bundled firmware only, not capability proof"))
    parser.add_argument("--arc-metadata", action="store_true", help=(
        "validate stored ARC compiler hints and extension declarations; bundled firmware only, not ISA proof"))
    parser.add_argument("--csc-command", action="store_true", help=(
        "validate the fixed local CSC command fallback path; bundled firmware only, not completion or capability proof"))
    parser.add_argument("--command-buffer-bridge", action="store_true", help=(
        "validate the initialized outer packet address/loader relocation bridge; bundled firmware only, not runtime proof"))
    parser.add_argument("--inner-descriptor", action="store_true", help=(
        "validate two fixed pre-relocation descriptor field paths; bundled firmware only, conditional ARC interpretation"))
    parser.add_argument("--scaler-fir", action="store_true", help=(
        "validate fixed stock A32 scaler routes and FIR tables; bundled firmware only, not hardware coefficient format proof"))
    parser.add_argument("--ppb-handoff", action="store_true", help=(
        "validate stock A32 metadata acquire/peek/return bodies; not a raw-surface lease"))
    parser.add_argument("--ppb-source", action="store_true", help=(
        "validate conditional ordinary H264 source equations; bundled firmware only, not a source-plane lease"))
    parser.add_argument("--ppb-saved-context", action="store_true", help=(
        "validate the conditional saved ARC context bridge; bundled firmware only, not active state or a lease"))
    parser.add_argument("--ppb-stop-context", action="store_true", help=(
        "validate conditional STOP/save ordering and status masking; not backend completion or a lease"))
    parser.add_argument("--ppb-fixed-metadata", action="store_true", help=(
        "validate fixed per-picture metadata publications and ring headers; not source ownership or a lease"))
    parser.add_argument("--ppb-return-header", action="store_true", help=(
        "validate bounded handle-route and sequential return-header observations; not routing or completion proof"))
    parser.add_argument("--ppb-bank-ledger", nargs=55, metavar="DWORD",
                        type=lambda word: _ppb_bank_u32(int(word, 0), "ledger word"), help=(
        "decode 55 previously captured flag/bank DWORDs; hexadecimal or decimal, offline only, not a lease"))
    parser.add_argument("--debug-mechanisms", action="store_true", help=(
        "separate fixed C011 DEBUG_SETUP, ARM/ARC UART and ARC DRAM-log evidence; "
        "bundled firmware only, not runtime accessibility"))
    parser.add_argument("--rx-descriptor-admission", action="store_true", help=(
        "validate fixed Y-RX descriptor publication; bundled firmware only, not DMA completion"))
    parser.add_argument("--channel-fields", action="store_true", help=(
        "inventory selected fixed A32 channel-field accesses; not whole-image alias recovery"))
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.expect_sha256):
        parser.error("--expect-sha256 must be 64 hexadecimal digits")
    try:
        report = analyze(read_firmware(args.firmware), args.symbol or DEFAULT_SYMBOLS,
                         args.expect_sha256.lower(), args.references, args.all_symbols, args.bootstrap,
                         args.picture_output, args.arc_metadata, args.csc_command, args.command_buffer_bridge,
                         args.inner_descriptor, args.scaler_fir, args.ppb_handoff, args.ppb_source,
                         args.ppb_saved_context, args.ppb_stop_context, args.ppb_fixed_metadata, args.ppb_return_header,
                         args.ppb_bank_ledger, args.debug_mechanisms, args.rx_descriptor_admission,
                         args.channel_fields)
    except (OSError, FormatError) as error:
        print(f"flea_fw_map: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
