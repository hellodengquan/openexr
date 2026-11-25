// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenEXR Project.

#ifndef OPENEXR_CORE_TEST_PIZ_INTEGRATION_H
#define OPENEXR_CORE_TEST_PIZ_INTEGRATION_H

#include <string>

// Lossless PIZ round trip over HALF / UINT / FLOAT channels with mixed
// subsampling: decoded samples must match the stored raw bit patterns,
// channel order and actual sampling positions exactly.
void testPIZBitPatterns (const std::string& tempdir);

// Partial availability of PIZ compression blocks: intact chunks must stay
// independently reconstructable, while missing or corrupt chunks must be
// reported as errors without taking the good regions down with them.
void testPIZPartialChunks (const std::string& tempdir);

#endif // OPENEXR_CORE_TEST_PIZ_INTEGRATION_H
