/*
 * Copyright (c)2013-2020 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2024-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "Constants.hpp"
#include "Identity.hpp"
#include "SHA512.hpp"
#include "Salsa20.hpp"
#include "Utils.hpp"
#include "Speck128.hpp"

#include <cstring>
#include <cstdint>
#include <algorithm>

namespace ZeroTier {

namespace {

// This is the memory-intensive hash function used to compute v0 identities from v0 public keys.
#define ZT_V0_IDENTITY_GEN_MEMORY 2097152
void identityV0ProofOfWorkFrankenhash(const void *const publicKey,unsigned int publicKeyBytes,void *const digest,void *const genmem) noexcept
{
	// Digest publicKey[] to obtain initial digest
	SHA512(digest,publicKey,publicKeyBytes);

	// Initialize genmem[] using Salsa20 in a CBC-like configuration since
	// ordinary Salsa20 is randomly seek-able. This is good for a cipher
	// but is not what we want for sequential memory-hardness.
	Utils::zero<ZT_V0_IDENTITY_GEN_MEMORY>(genmem);
	Salsa20 s20(digest,(char *)digest + 32);
	s20.crypt20((char *)genmem,(char *)genmem,64);
	for(unsigned long i=64;i<ZT_V0_IDENTITY_GEN_MEMORY;i+=64) {
		unsigned long k = i - 64;
		*((uint64_t *)((char *)genmem + i)) = *((uint64_t *)((char *)genmem + k));
		*((uint64_t *)((char *)genmem + i + 8)) = *((uint64_t *)((char *)genmem + k + 8));
		*((uint64_t *)((char *)genmem + i + 16)) = *((uint64_t *)((char *)genmem + k + 16));
		*((uint64_t *)((char *)genmem + i + 24)) = *((uint64_t *)((char *)genmem + k + 24));
		*((uint64_t *)((char *)genmem + i + 32)) = *((uint64_t *)((char *)genmem + k + 32));
		*((uint64_t *)((char *)genmem + i + 40)) = *((uint64_t *)((char *)genmem + k + 40));
		*((uint64_t *)((char *)genmem + i + 48)) = *((uint64_t *)((char *)genmem + k + 48));
		*((uint64_t *)((char *)genmem + i + 56)) = *((uint64_t *)((char *)genmem + k + 56));
		s20.crypt20((char *)genmem + i,(char *)genmem + i,64);
	}

	// Render final digest using genmem as a lookup table
	for(unsigned long i=0;i<(ZT_V0_IDENTITY_GEN_MEMORY / sizeof(uint64_t));) {
		unsigned long idx1 = (unsigned long)(Utils::ntoh(((uint64_t *)genmem)[i++]) % (64 / sizeof(uint64_t))); // NOLINT(hicpp-use-auto,modernize-use-auto)
		unsigned long idx2 = (unsigned long)(Utils::ntoh(((uint64_t *)genmem)[i++]) % (ZT_V0_IDENTITY_GEN_MEMORY / sizeof(uint64_t))); // NOLINT(hicpp-use-auto,modernize-use-auto)
		uint64_t tmp = ((uint64_t *)genmem)[idx2];
		((uint64_t *)genmem)[idx2] = ((uint64_t *)digest)[idx1];
		((uint64_t *)digest)[idx1] = tmp;
		s20.crypt20(digest,digest,64);
	}
}
struct identityV0ProofOfWorkCriteria
{
	ZT_INLINE identityV0ProofOfWorkCriteria(unsigned char *sb,char *gm) noexcept : digest(sb),genmem(gm) {}
	ZT_INLINE bool operator()(const uint8_t pub[ZT_C25519_COMBINED_PUBLIC_KEY_SIZE]) const noexcept
	{
		identityV0ProofOfWorkFrankenhash(pub,ZT_C25519_COMBINED_PUBLIC_KEY_SIZE,digest,genmem);
		return (digest[0] < 17);
	}
	unsigned char *digest;
	char *genmem;
};

// This is a simpler memory-intensive hash function for V1 identity generation.
// It's not quite as intensive as the V0 frankenhash, is a little more orderly in
// its design, but remains relatively resistant to GPU acceleration due to memory
// requirements for efficient computation.
#define ZT_IDENTITY_V1_POW_MEMORY_SIZE 98304
bool identityV1ProofOfWorkCriteria(const void *in,const unsigned int len,uint64_t *const b)
{
	SHA512(b,in,len);

	// This treats hash output as little-endian, so swap on BE machines.
#if __BYTE_ORDER == __BIG_ENDIAN
	b[0] = Utils::swapBytes(b[0]);
	b[1] = Utils::swapBytes(b[1]);
	b[2] = Utils::swapBytes(b[2]);
	b[3] = Utils::swapBytes(b[3]);
	b[4] = Utils::swapBytes(b[4]);
	b[5] = Utils::swapBytes(b[5]);
	b[6] = Utils::swapBytes(b[6]);
	b[7] = Utils::swapBytes(b[7]);
#endif

	// Memory-intensive work: fill 'b' with pseudo-random bits generated from
	// a reduced-round instance of Speck128 using a CBC-like construction.
	// Then sort the resulting integer array in ascending numerical order.
	// The sort requires that we compute and cache the whole data set, or at
	// least that this is the most efficient implementation.
	Speck128<24> s16;
	s16.initXY(b[4],b[5]);
	for(unsigned long i=0;i<(ZT_IDENTITY_V1_POW_MEMORY_SIZE-8);) {
		// Load four 128-bit blocks.
		uint64_t x0 = b[i];
		uint64_t y0 = b[i + 1];
		uint64_t x1 = b[i + 2];
		uint64_t y1 = b[i + 3];
		uint64_t x2 = b[i + 4];
		uint64_t y2 = b[i + 5];
		uint64_t x3 = b[i + 6];
		uint64_t y3 = b[i + 7];

		// Advance by 512 bits / 64 bytes (its a uint64_t array).
		i += 8;

		// Ensure that mixing happens across blocks.
		x0 += x1;
		x1 += x2;
		x2 += x3;
		x3 += y0;

		// Encrypt 4X blocks. Speck is used for this PoW function because
		// its performance is similar on all architectures while AES is much
		// faster on some than others.
		s16.encryptXYXYXYXY(x0,y0,x1,y1,x2,y2,x3,y3);

		// Store four 128-bit blocks at new position.
		b[i] = x0;
		b[i + 1] = y0;
		b[i + 2] = x1;
		b[i + 3] = y1;
		b[i + 4] = x2;
		b[i + 5] = y2;
		b[i + 6] = x3;
		b[i + 7] = y3;
	}

	// Sort array, something that can't efficiently be done unless we have
	// computed the whole array and have it in memory. This also involves
	// branching which is less efficient on GPUs.
	std::sort(b,b + ZT_IDENTITY_V1_POW_MEMORY_SIZE);

	// Swap byte order back on BE machines.
#if __BYTE_ORDER == __BIG_ENDIAN
	for(unsigned int i=0;i<98304;i+=8) {
		b[i] = Utils::swapBytes(b[i]);
		b[i + 1] = Utils::swapBytes(b[i + 1]);
		b[i + 2] = Utils::swapBytes(b[i + 2]);
		b[i + 3] = Utils::swapBytes(b[i + 3]);
		b[i + 4] = Utils::swapBytes(b[i + 4]);
		b[i + 5] = Utils::swapBytes(b[i + 5]);
		b[i + 6] = Utils::swapBytes(b[i + 6]);
		b[i + 7] = Utils::swapBytes(b[i + 7]);
	}
#endif

	// Hash resulting sorted array to get final result for PoW criteria test.
	SHA384(b,b,sizeof(b),in,len);

	// PoW passes if sum of first two 64-bit integers (treated as little-endian) mod 180 is 0.
	// This value was picked to yield about 1-2s total on typical desktop and server cores in 2020.
#if __BYTE_ORDER == __BIG_ENDIAN
	const uint64_t finalHash = Utils::swapBytes(b[0]) + Utils::swapBytes(b[1]);
#else
	const uint64_t finalHash = b[0] + b[1];
#endif
	return (finalHash % 180U) == 0;
}

} // anonymous namespace

const Identity Identity::NIL;

bool Identity::generate(const Type t)
{
	_type = t;
	_hasPrivate = true;

	switch(t) {
		case C25519: {
			// Generate C25519/Ed25519 key pair whose hash satisfies a "hashcash" criterion and generate the
			// address from the last 40 bits of this hash. This is different from the fingerprint hash for V0.
			uint8_t digest[64];
			char *const genmem = new char[ZT_V0_IDENTITY_GEN_MEMORY];
			do {
				C25519::generateSatisfying(identityV0ProofOfWorkCriteria(digest,genmem),_pub.c25519,_priv.c25519);
				_address.setTo(digest + 59);
			} while (_address.isReserved());
			delete[] genmem;
			_computeHash();
		} break;

		case P384: {
			uint64_t *const b = (uint64_t *)malloc(ZT_IDENTITY_V1_POW_MEMORY_SIZE * 8); // NOLINT(hicpp-use-auto,modernize-use-auto)
			if (!b)
				return false;
			for(;;) {
				// Loop until we pass the PoW criteria. The nonce is only 8 bits, so generate
				// some new key material every time it wraps. The ECC384 generator is slightly
				// faster so use that one.
				_pub.nonce = 0;
				C25519::generate(_pub.c25519,_priv.c25519);
				ECC384GenerateKey(_pub.p384,_priv.p384);
				for(;;) {
					if (identityV1ProofOfWorkCriteria(&_pub,sizeof(_pub),b))
						break;
					if (++_pub.nonce == 0)
						ECC384GenerateKey(_pub.p384,_priv.p384);
				}

				// If we passed PoW then check that the address is valid, otherwise loop
				// back around and run the whole process again.
				_computeHash();
				_address.setTo(_fp.hash());
				if (!_address.isReserved())
					break;
			}
			free(b);
		} break;

		default:
			return false;
	}

	return true;
}

bool Identity::locallyValidate() const noexcept
{
	try {
		if ((!_address.isReserved()) && (_address)) {
			switch (_type) {

				case C25519: {
					uint8_t digest[64];
					char *genmem = new char[ZT_V0_IDENTITY_GEN_MEMORY];
					identityV0ProofOfWorkFrankenhash(_pub.c25519,ZT_C25519_COMBINED_PUBLIC_KEY_SIZE,digest,genmem);
					delete[] genmem;
					return ((_address == Address(digest + 59)) && (digest[0] < 17));
				}

				case P384: {
					if (_address != Address(_fp.hash()))
						return false;
					uint64_t *const b = (uint64_t *)malloc(ZT_IDENTITY_V1_POW_MEMORY_SIZE * 8); // NOLINT(hicpp-use-auto,modernize-use-auto)
					if (!b)
						return false;
					const bool ok = identityV1ProofOfWorkCriteria(&_pub,sizeof(_pub),b);
					free(b);
					return ok;
				}

			}
		}
	} catch ( ... ) {}
	return false;
}

void Identity::hashWithPrivate(uint8_t h[ZT_IDENTITY_HASH_SIZE]) const
{
	if (_hasPrivate) {
		switch (_type) {

			case C25519:
				SHA384(h,_pub.c25519,ZT_C25519_COMBINED_PUBLIC_KEY_SIZE,_priv.c25519,ZT_C25519_COMBINED_PRIVATE_KEY_SIZE);
				break;

			case P384:
				SHA384(h,&_pub,sizeof(_pub),&_priv,sizeof(_priv));
				break;

		}
		return;
	}
	Utils::zero<48>(h);
}

unsigned int Identity::sign(const void *data,unsigned int len,void *sig,unsigned int siglen) const
{
	if (_hasPrivate) {
		switch(_type) {

			case C25519:
				if (siglen >= ZT_C25519_SIGNATURE_LEN) {
					C25519::sign(_priv.c25519,_pub.c25519,data,len,sig);
					return ZT_C25519_SIGNATURE_LEN;
				}

			case P384:
				if (siglen >= ZT_ECC384_SIGNATURE_SIZE) {
					uint8_t h[48];
					SHA384(h,data,len,&_pub,ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE); // include C25519 public key in hash
					ECC384ECDSASign(_priv.p384,h,(uint8_t *)sig);
					return ZT_ECC384_SIGNATURE_SIZE;
				}

		}
	}
	return 0;
}

bool Identity::verify(const void *data,unsigned int len,const void *sig,unsigned int siglen) const
{
	switch(_type) {

		case C25519:
			return C25519::verify(_pub.c25519,data,len,sig,siglen);

		case P384:
			if (siglen == ZT_ECC384_SIGNATURE_SIZE) {
				uint8_t h[48];
				SHA384(h,data,len,&_pub,ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE);
				return ECC384ECDSAVerify(_pub.p384,h,(const uint8_t *)sig);
			}
			break;

	}
	return false;
}

bool Identity::agree(const Identity &id,uint8_t key[ZT_PEER_SECRET_KEY_LENGTH]) const
{
	uint8_t rawkey[128];
	uint8_t h[64];
	if (_hasPrivate) {
		if (_type == C25519) {

			if ((id._type == C25519)||(id._type == P384)) {
				// If we are a C25519 key we can agree with another C25519 key or with only the
				// C25519 portion of a type 1 P-384 key.
				C25519::agree(_priv.c25519,id._pub.c25519,rawkey);
				SHA512(h,rawkey,ZT_C25519_ECDH_SHARED_SECRET_SIZE);
				Utils::copy<ZT_PEER_SECRET_KEY_LENGTH>(key,h);
				return true;
			}

		} else if (_type == P384) {

			if (id._type == P384) {
				// For another P384 identity we execute DH agreement with BOTH keys and then
				// hash the results together. For those (cough FIPS cough) who only consider
				// P384 to be kosher, the C25519 secret can be considered a "salt"
				// or something. For those who don't trust P384 this means the privacy of
				// your traffic is also protected by C25519.
				C25519::agree(_priv.c25519,id._pub.c25519,rawkey);
				ECC384ECDH(id._pub.p384,_priv.p384,rawkey + ZT_C25519_ECDH_SHARED_SECRET_SIZE);
				SHA384(h,rawkey,ZT_C25519_ECDH_SHARED_SECRET_SIZE + ZT_ECC384_SHARED_SECRET_SIZE);
				Utils::copy<ZT_PEER_SECRET_KEY_LENGTH>(key,h);
				return true;
			} else if (id._type == C25519) {
				// If the other identity is a C25519 identity we can agree using only that type.
				C25519::agree(_priv.c25519,id._pub.c25519,rawkey);
				SHA512(h,rawkey,ZT_C25519_ECDH_SHARED_SECRET_SIZE);
				Utils::copy<ZT_PEER_SECRET_KEY_LENGTH>(key,h);
				return true;
			}

		}
	}
	return false;
}

char *Identity::toString(bool includePrivate,char buf[ZT_IDENTITY_STRING_BUFFER_LENGTH]) const
{
	char *p = buf;
	_address.toString(p);
	p += 10;
	*(p++) = ':';

	switch(_type) {

		case C25519: {
			*(p++) = '0';
			*(p++) = ':';
			Utils::hex(_pub.c25519,ZT_C25519_COMBINED_PUBLIC_KEY_SIZE,p);
			p += ZT_C25519_COMBINED_PUBLIC_KEY_SIZE * 2;
			if ((_hasPrivate)&&(includePrivate)) {
				*(p++) = ':';
				Utils::hex(_priv.c25519,ZT_C25519_COMBINED_PRIVATE_KEY_SIZE,p);
				p += ZT_C25519_COMBINED_PRIVATE_KEY_SIZE * 2;
			}
			*p = (char)0;
			return buf;
		}

		case P384: {
			*(p++) = '1';
			*(p++) = ':';
			int el = Utils::b32e((const uint8_t *)(&_pub),sizeof(_pub),p,(int)(ZT_IDENTITY_STRING_BUFFER_LENGTH - (uintptr_t)(p - buf)));
			if (el <= 0) return nullptr;
			p += el;
			if ((_hasPrivate)&&(includePrivate)) {
				*(p++) = ':';
				el = Utils::b32e((const uint8_t *)(&_priv),sizeof(_priv),p,(int)(ZT_IDENTITY_STRING_BUFFER_LENGTH - (uintptr_t)(p - buf)));
				if (el <= 0) return nullptr;
				p += el;
			}
			*p = (char)0;
			return buf;
		}

	}

	return nullptr;
}

bool Identity::fromString(const char *str)
{
	_fp.zero();
	_hasPrivate = false;

	if (!str) {
		_address.zero();
		return false;
	}

	char tmp[ZT_IDENTITY_STRING_BUFFER_LENGTH];
	if (!Utils::scopy(tmp,sizeof(tmp),str)) {
		_address.zero();
		return false;
	}

	int fno = 0;
	char *saveptr = nullptr;
	for(char *f=Utils::stok(tmp,":",&saveptr);((f)&&(fno < 4));f=Utils::stok(nullptr,":",&saveptr)) {
		switch(fno++) {

			case 0:
				_address = Address(Utils::hexStrToU64(f));
				if (_address.isReserved()) {
					_address.zero();
					return false;
				}
				break;

			case 1:
				if ((f[0] == '0')&&(!f[1])) {
					_type = C25519;
				} else if ((f[0] == '1')&&(!f[1])) {
					_type = P384;
				} else {
					_address.zero();
					return false;
				}
				break;

			case 2:
				switch(_type) {

					case C25519:
						if (Utils::unhex(f,strlen(f),_pub.c25519,ZT_C25519_COMBINED_PUBLIC_KEY_SIZE) != ZT_C25519_COMBINED_PUBLIC_KEY_SIZE) {
							_address.zero();
							return false;
						}
						break;

					case P384:
						if (Utils::b32d(f,(uint8_t *)(&_pub),sizeof(_pub)) != sizeof(_pub)) {
							_address.zero();
							return false;
						}
						break;

				}
				break;

			case 3:
				if (strlen(f) > 1) {
					switch(_type) {

						case C25519:
							if (Utils::unhex(f,strlen(f),_priv.c25519,ZT_C25519_COMBINED_PRIVATE_KEY_SIZE) != ZT_C25519_COMBINED_PRIVATE_KEY_SIZE) {
								_address.zero();
								return false;
							} else {
								_hasPrivate = true;
							}
							break;

						case P384:
							if (Utils::b32d(f,(uint8_t *)(&_priv),sizeof(_priv)) != sizeof(_priv)) {
								_address.zero();
								return false;
							} else {
								_hasPrivate = true;
							}
							break;

					}
					break;
				}

		}
	}

	if (fno < 3) {
		_address.zero();
		return false;
	}

	_computeHash();
	if ((_type == P384)&&(_address != Address(_fp.hash()))) {
		_address.zero();
		return false;
	}

	return true;
}

int Identity::marshal(uint8_t data[ZT_IDENTITY_MARSHAL_SIZE_MAX],const bool includePrivate) const noexcept
{
	_address.copyTo(data);
	switch(_type) {
		case C25519:
			data[ZT_ADDRESS_LENGTH] = (uint8_t)C25519;
			Utils::copy<ZT_C25519_COMBINED_PUBLIC_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1,_pub.c25519);
			if ((includePrivate)&&(_hasPrivate)) {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE] = ZT_C25519_COMBINED_PRIVATE_KEY_SIZE;
				Utils::copy<ZT_C25519_COMBINED_PRIVATE_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1,_priv.c25519);
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1 + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE;
			} else {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE] = 0;
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1;
			}

		case P384:
			data[ZT_ADDRESS_LENGTH] = (uint8_t)P384;
			Utils::copy<ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1,&_pub);
			if ((includePrivate)&&(_hasPrivate)) {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE] = ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE;
				Utils::copy<ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE>(data + ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1,&_priv);
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1 + ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE;
			} else {
				data[ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE] = 0;
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1;
			}

	}
	return -1;
}

int Identity::unmarshal(const uint8_t *data,const int len) noexcept
{
	_fp.zero();
	_hasPrivate = false;

	if (len < (1 + ZT_ADDRESS_LENGTH))
		return -1;
	_address.setTo(data);

	unsigned int privlen;
	switch((_type = (Type)data[ZT_ADDRESS_LENGTH])) {

		case C25519:
			if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1))
				return -1;

			Utils::copy<ZT_C25519_COMBINED_PUBLIC_KEY_SIZE>(_pub.c25519,data + ZT_ADDRESS_LENGTH + 1);
			_computeHash();

			privlen = data[ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE];
			if (privlen == ZT_C25519_COMBINED_PRIVATE_KEY_SIZE) {
				if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1 + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE))
					return -1;
				_hasPrivate = true;
				Utils::copy<ZT_C25519_COMBINED_PRIVATE_KEY_SIZE>(_priv.c25519,data + ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1);
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1 + ZT_C25519_COMBINED_PRIVATE_KEY_SIZE;
			} else if (privlen == 0) {
				_hasPrivate = false;
				return ZT_ADDRESS_LENGTH + 1 + ZT_C25519_COMBINED_PUBLIC_KEY_SIZE + 1;
			}
			break;

		case P384:
			if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1))
				return -1;

			Utils::copy<ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE>(&_pub,data + ZT_ADDRESS_LENGTH + 1);
			_computeHash(); // this sets the address for P384
			if (_address != Address(_fp.hash())) // this sanity check is possible with V1 identities
				return -1;

			privlen = data[ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE];
			if (privlen == ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE) {
				if (len < (ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1 + ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE))
					return -1;
				_hasPrivate = true;
				Utils::copy<ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE>(&_priv,data + ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1);
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1 + ZT_IDENTITY_P384_COMPOUND_PRIVATE_KEY_SIZE;
			} else if (privlen == 0) {
				_hasPrivate = false;
				return ZT_ADDRESS_LENGTH + 1 + ZT_IDENTITY_P384_COMPOUND_PUBLIC_KEY_SIZE + 1;
			}
			break;

	}

	return -1;
}

void Identity::_computeHash()
{
	switch(_type) {
		default:
			_fp.zero();
			break;

		case C25519:
			_fp._fp.address = _address.toInt();
			SHA384(_fp._fp.hash,_pub.c25519,ZT_C25519_COMBINED_PUBLIC_KEY_SIZE);
			break;

		case P384:
			SHA384(_fp._fp.hash,&_pub,sizeof(_pub));
			_fp._fp.address = _address.toInt();
			break;
	}
}

} // namespace ZeroTier

extern "C" {

ZT_Identity *ZT_Identity_new(enum ZT_Identity_Type type)
{
	if ((type != ZT_IDENTITY_TYPE_C25519)&&(type != ZT_IDENTITY_TYPE_P384))
		return nullptr;
	try {
		ZeroTier::Identity *const id = new ZeroTier::Identity(); // NOLINT(hicpp-use-auto,modernize-use-auto)
		id->generate((ZeroTier::Identity::Type)type);
		return reinterpret_cast<ZT_Identity *>(id);
	} catch ( ... ) {
		return nullptr;
	}
}

ZT_Identity *ZT_Identity_fromString(const char *idStr)
{
	if (!idStr)
		return nullptr;
	try {
		ZeroTier::Identity *const id = new ZeroTier::Identity(); // NOLINT(hicpp-use-auto,modernize-use-auto)
		if (!id->fromString(idStr)) {
			delete id;
			return nullptr;
		}
		return reinterpret_cast<ZT_Identity *>(id);
	} catch ( ... ) {
		return nullptr;
	}
}

int ZT_Identity_validate(const ZT_Identity *id)
{
	if (!id)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->locallyValidate() ? 1 : 0;
}

unsigned int ZT_Identity_sign(const ZT_Identity *id,const void *data,unsigned int len,void *signature,unsigned int signatureBufferLength)
{
	if (!id)
		return 0;
	if (signatureBufferLength < ZT_SIGNATURE_BUFFER_SIZE)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->sign(data,len,signature,signatureBufferLength);
}

int ZT_Identity_verify(const ZT_Identity *id,const void *data,unsigned int len,const void *signature,unsigned int sigLen)
{
	if ((!id)||(!signature)||(!sigLen))
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->verify(data,len,signature,sigLen) ? 1 : 0;
}

enum ZT_Identity_Type ZT_Identity_type(const ZT_Identity *id)
{
	if (!id)
		return (ZT_Identity_Type)0;
	return (enum ZT_Identity_Type)reinterpret_cast<const ZeroTier::Identity *>(id)->type();
}

char *ZT_Identity_toString(const ZT_Identity *id,char *buf,int capacity,int includePrivate)
{
	if ((!id)||(!buf)||(capacity < ZT_IDENTITY_STRING_BUFFER_LENGTH))
		return nullptr;
	reinterpret_cast<const ZeroTier::Identity *>(id)->toString(includePrivate != 0,buf);
	return buf;
}

int ZT_Identity_hasPrivate(const ZT_Identity *id)
{
	if (!id)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->hasPrivate() ? 1 : 0;
}

uint64_t ZT_Identity_address(const ZT_Identity *id)
{
	if (!id)
		return 0;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->address().toInt();
}

const ZT_Fingerprint *ZT_Identity_fingerprint(const ZT_Identity *id)
{
	if (!id)
		return nullptr;
	return reinterpret_cast<const ZeroTier::Identity *>(id)->fingerprint().apiFingerprint();
}

ZT_SDK_API void ZT_Identity_delete(ZT_Identity *id)
{
	if (id)
		delete reinterpret_cast<ZeroTier::Identity *>(id);
}

}
