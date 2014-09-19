/*
 * Secure Remote Password 6a implementation
 * Copyright (c) 2010 Tom Cocagne. All rights reserved.
 * https://github.com/cocagne/csrp
 *
 * The MIT License (MIT)
 * 
 * Copyright (c) 2013 Tom Cocagne
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 */

#ifdef WIN32
    #include <Wincrypt.h>
#else
    #include <sys/time.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>


#include "srp.h"

static int g_initialized = 0;


struct NGHex 
{
    const char * n_hex;
    const char * g_hex;
};

/* All constants here were pulled from Appendix A of RFC 5054 */
static struct NGHex global_Ng_constants[] = {
 { /* 3072 */
	 "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
	 "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
	 "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
	 "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
	 "49286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8"
	 "FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D"
	 "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C"
	 "180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
	 "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D"
	 "04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7D"
	 "B3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D226"
	 "1AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
	 "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFC"
	 "E0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF",
	 "5"
 },
 {0,0} /* null sentinel */
};


static NGConstant * new_ng( SRP_NGType ng_type, const char * n_hex, const char * g_hex )
{
    NGConstant * ng   = (NGConstant *) malloc( sizeof(NGConstant) );
    ng->N             = BN_new();
    ng->g             = BN_new();

    if( !ng || !ng->N || !ng->g )
       return 0;

    if ( ng_type != SRP_NG_CUSTOM )
    {
        n_hex = global_Ng_constants[ ng_type ].n_hex;
        g_hex = global_Ng_constants[ ng_type ].g_hex;
    }
        
    BN_hex2bn( &ng->N, n_hex );
    BN_hex2bn( &ng->g, g_hex );
    
    return ng;
}

static void delete_ng( NGConstant * ng )
{
   if (ng)
   {
      BN_free( ng->N );
      BN_free( ng->g );
      ng->N = 0;
      ng->g = 0;
      free(ng);
   }
}



typedef union 
{
    SHA_CTX    sha;
    SHA256_CTX sha256;
    SHA512_CTX sha512;
} HashCTX;


struct SRPUser
{
    SRP_HashAlgorithm  hash_alg;
    NGConstant        *ng;
    
    BIGNUM *a;
    BIGNUM *A;
    BIGNUM *S;

    const unsigned char * bytes_A;
    int                   authenticated;
    
    const char *          username;
    const unsigned char * password;
    int                   password_len;
    
    unsigned char M           [SHA512_DIGEST_LENGTH];
    unsigned char H_AMK       [SHA512_DIGEST_LENGTH];
    unsigned char session_key [SHA512_DIGEST_LENGTH];
};


static int hash_init( SRP_HashAlgorithm alg, HashCTX *c )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA1_Init( &c->sha );
      case SRP_SHA224: return SHA224_Init( &c->sha256 );
      case SRP_SHA256: return SHA256_Init( &c->sha256 );
      case SRP_SHA384: return SHA384_Init( &c->sha512 );
      case SRP_SHA512: return SHA512_Init( &c->sha512 );
      default:
        return -1;
    };
}
static int hash_update( SRP_HashAlgorithm alg, HashCTX *c, const void *data, size_t len )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA1_Update( &c->sha, data, len );
      case SRP_SHA224: return SHA224_Update( &c->sha256, data, len );
      case SRP_SHA256: return SHA256_Update( &c->sha256, data, len );
      case SRP_SHA384: return SHA384_Update( &c->sha512, data, len );
      case SRP_SHA512: return SHA512_Update( &c->sha512, data, len );
      default:
        return -1;
    };
}
static int hash_final( SRP_HashAlgorithm alg, HashCTX *c, unsigned char *md )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA1_Final( md, &c->sha );
      case SRP_SHA224: return SHA224_Final( md, &c->sha256 );
      case SRP_SHA256: return SHA256_Final( md, &c->sha256 );
      case SRP_SHA384: return SHA384_Final( md, &c->sha512 );
      case SRP_SHA512: return SHA512_Final( md, &c->sha512 );
      default:
        return -1;
    };
}
static unsigned char * hash( SRP_HashAlgorithm alg, const unsigned char *d, size_t n, unsigned char *md )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA1( d, n, md );
      case SRP_SHA224: return SHA224( d, n, md );
      case SRP_SHA256: return SHA256( d, n, md );
      case SRP_SHA384: return SHA384( d, n, md );
      case SRP_SHA512: return SHA512( d, n, md );
      default:
        return 0;
    };
}
static int hash_length( SRP_HashAlgorithm alg )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA_DIGEST_LENGTH;
      case SRP_SHA224: return SHA224_DIGEST_LENGTH;
      case SRP_SHA256: return SHA256_DIGEST_LENGTH;
      case SRP_SHA384: return SHA384_DIGEST_LENGTH;
      case SRP_SHA512: return SHA512_DIGEST_LENGTH;
      default:
        return -1;
    };
}


static BIGNUM * H_nn( SRP_HashAlgorithm alg, const BIGNUM * n1, const BIGNUM * n2 )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_n1 = BN_num_bytes(n1);
    int             len_n2 = BN_num_bytes(n2);
    int             nbytes = len_n1 + len_n2;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;
    BN_bn2bin(n1, bin);
    BN_bn2bin(n2, bin + len_n1);
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * H_ns( SRP_HashAlgorithm alg, const BIGNUM * n, const unsigned char * bytes, int len_bytes )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_n  = BN_num_bytes(n);
    int             nbytes = len_n + len_bytes;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;
    BN_bn2bin(n, bin);
    memcpy( bin + len_n, bytes, len_bytes );
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}
    
static BIGNUM * calculate_x( SRP_HashAlgorithm alg, const BIGNUM * salt, const char * username, const unsigned char * password, int password_len )
{
    unsigned char ucp_hash[SHA512_DIGEST_LENGTH];
    HashCTX       ctx;

    hash_init( alg, &ctx );

    hash_update( alg, &ctx, username, strlen(username) );
    hash_update( alg, &ctx, ":", 1 );
    hash_update( alg, &ctx, password, password_len );
    
    hash_final( alg, &ctx, ucp_hash );
        
    return H_ns( alg, salt, ucp_hash, hash_length(alg) );
}

static void update_hash_n( SRP_HashAlgorithm alg, HashCTX *ctx, const BIGNUM * n )
{
    unsigned long len = BN_num_bytes(n);
    unsigned char * n_bytes = (unsigned char *) malloc( len );
    if (!n_bytes)
       return;
    BN_bn2bin(n, n_bytes);
    hash_update(alg, ctx, n_bytes, len);
    free(n_bytes);
}

static void hash_num( SRP_HashAlgorithm alg, const BIGNUM * n, unsigned char * dest )
{
    int             nbytes = BN_num_bytes(n);
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if(!bin)
       return;
    BN_bn2bin(n, bin);
    hash( alg, bin, nbytes, dest );
    free(bin);
}

static void calculate_M( SRP_HashAlgorithm alg, NGConstant *ng, unsigned char * dest, const char * I, const BIGNUM * s,
                         const BIGNUM * A, const BIGNUM * B, const unsigned char * K )
{
    unsigned char H_N[ SHA512_DIGEST_LENGTH ];
    unsigned char H_g[ SHA512_DIGEST_LENGTH ];
    unsigned char H_I[ SHA512_DIGEST_LENGTH ];
    unsigned char H_xor[ SHA512_DIGEST_LENGTH ];
    HashCTX       ctx;
    int           i = 0;
    int           hash_len = hash_length(alg);
        
    hash_num( alg, ng->N, H_N );
    hash_num( alg, ng->g, H_g );
    
    hash(alg, (const unsigned char *)I, strlen(I), H_I);
    
    
    for (i=0; i < hash_len; i++ )
        H_xor[i] = H_N[i] ^ H_g[i];
    
    hash_init( alg, &ctx );
    
    hash_update( alg, &ctx, H_xor, hash_len );
    hash_update( alg, &ctx, H_I,   hash_len );
    update_hash_n( alg, &ctx, s );
    update_hash_n( alg, &ctx, A );
    update_hash_n( alg, &ctx, B );
    hash_update( alg, &ctx, K, hash_len );
    
    hash_final( alg, &ctx, dest );
}

static void calculate_H_AMK( SRP_HashAlgorithm alg, unsigned char *dest, const BIGNUM * A, const unsigned char * M, const unsigned char * K )
{
    HashCTX ctx;
    
    hash_init( alg, &ctx );
    
    update_hash_n( alg, &ctx, A );
    hash_update( alg, &ctx, M, hash_length(alg) );
    hash_update( alg, &ctx, K, hash_length(alg) );
    
    hash_final( alg, &ctx, dest );
}


static void init_random()
{    
    if (g_initialized)
        return;
    
#ifdef WIN32
    HCRYPTPROV wctx;
#else
    FILE   *fp   = 0;
#endif
    
    unsigned char buff[64];

    
#ifdef WIN32

        CryptAcquireContext(&wctx, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        
        CryptGenRandom(wctx, sizeof(buff), (BYTE*) buff);
        
        CryptReleaseContext(wctx, 0);

        g_initialized = 1;
        
#else
        fp = fopen("/dev/urandom", "r");
        
        if (fp)
        {
            fread(buff, sizeof(buff), 1, fp);
            fclose(fp);
            g_initialized = 1;
        }
#endif

    if (g_initialized)
       RAND_seed( buff, sizeof(buff) );
}


/***********************************************************************************************************
 *
 *  Exported Functions
 *
 ***********************************************************************************************************/

void srp_random_seed( const unsigned char * random_data, int data_length )
{
    g_initialized = 1;

    if (random_data)
        RAND_seed( random_data, data_length );
}


void srp_create_salted_verification_key( SRP_HashAlgorithm alg, 
                                         SRP_NGType ng_type, const char * username,
                                         const unsigned char * password, int len_password,
                                         const unsigned char ** bytes_s, int * len_s, 
                                         const unsigned char ** bytes_v, int * len_v,
                                         const char * n_hex, const char * g_hex )
{
    BIGNUM     * s   = BN_new();
    BIGNUM     * v   = BN_new();
    BIGNUM     * x   = 0;
    BN_CTX     * ctx = BN_CTX_new();
    NGConstant * ng  = new_ng( ng_type, n_hex, g_hex );

    if( !s || !v || !ctx || !ng )
       goto cleanup_and_exit;

    init_random(); /* Only happens once */
    
    BN_rand(s, 32, -1, 0);
    
    x = calculate_x( alg, s, username, password, len_password );

    if( !x )
       goto cleanup_and_exit;

    BN_mod_exp(v, ng->g, x, ng->N, ctx);
        
    *len_s   = BN_num_bytes(s);
    *len_v   = BN_num_bytes(v);
    
    *bytes_s = (const unsigned char *) malloc( *len_s );
    *bytes_v = (const unsigned char *) malloc( *len_v );

    if (!bytes_s || !bytes_v)
       goto cleanup_and_exit;
    
    BN_bn2bin(s, (unsigned char *) *bytes_s);
    BN_bn2bin(v, (unsigned char *) *bytes_v);
    
 cleanup_and_exit:
    delete_ng( ng );
    BN_free(s);
    BN_free(v);
    BN_free(x);
    BN_CTX_free(ctx);
}

void srp_compute_shared_secret(SRPVerifier * ver, const unsigned char * bytes_A, int len_A)
{
	BIGNUM             *A = BN_bin2bn(bytes_A, len_A, NULL);
	BIGNUM             *B = BN_bin2bn(ver->bytes_B, ver->len_B, NULL);
	BIGNUM             *S = BN_new();	
	BIGNUM             *u = 0;
	BIGNUM             *tmp1 = BN_new();
	BIGNUM             *tmp2 = BN_new();
	BN_CTX             *ctx = BN_CTX_new();
		
	if (!A || !B || !S || !tmp1 || !tmp2 || !ctx)
		goto cleanup_and_exit;


	/* SRP-6a safety check */
	BN_mod(tmp1, A, ver->ng->N, ctx);
	if (!BN_is_zero(tmp1))
	{
		u = H_nn(ver->hash_alg, A, B);

		/* S = (A *(v^u)) ^ b */
		BN_mod_exp(tmp1, ver->v, u, ver->ng->N, ctx);
		BN_mul(tmp2, A, tmp1, ctx);
		BN_mod_exp(S, tmp2, ver->b, ver->ng->N, ctx);

		hash_num(ver->hash_alg, S, ver->session_key);

		calculate_M(ver->hash_alg, ver->ng, ver->M, ver->username, ver->s, A, B, ver->session_key);
		calculate_H_AMK(ver->hash_alg, ver->H_AMK, A, ver->M, ver->session_key);		
	}

cleanup_and_exit:		
	BN_free(A);
	if (u) BN_free(u);
	BN_free(B);
	BN_free(S);
	BN_free(tmp1);
	BN_free(tmp2);
	BN_CTX_free(ctx);

}



SRPVerifier * srp_create_salted_verifier( SRP_HashAlgorithm alg,
										  SRP_NGType ng_type, const char * username,
										  const unsigned char * password, int len_password,
										  const unsigned char ** bytes_s, int * len_s)
{		
	BIGNUM     * x = 0;
	BN_CTX     * ctx = BN_CTX_new();
	int          ulen = strlen(username) + 1;
	SRPVerifier *ver = 0;

	if ( !ctx )
		goto cleanup_and_exit;

	ver = new SRPVerifier;

	if (!ver)
		goto cleanup_and_exit;

	init_random(); /* Only happens once */

	ver->username = (char *)malloc(ulen);
	ver->hash_alg = alg;
	ver->ng = new_ng(ng_type, NULL, NULL);
	ver->s = BN_new();
	ver->v = BN_new();

	if (!ver->username)
	{
		delete ver;
		ver = 0;
		goto cleanup_and_exit;
	}

	memcpy((char*)ver->username, username, ulen);

	ver->authenticated = 0;

	BN_rand(ver->s, 128, -1, 0);

	x = calculate_x(alg, ver->s, username, password, len_password);

	if (!x)
		goto cleanup_and_exit;

	BN_mod_exp(ver->v, ver->ng->g, x, ver->ng->N, ctx);

	*len_s = BN_num_bytes(ver->s);	
	*bytes_s = (const unsigned char *)malloc(*len_s);
	
	if (!bytes_s )
		goto cleanup_and_exit;

	BN_bn2bin(ver->s, (unsigned char *)*bytes_s);	

cleanup_and_exit:		
	BN_free(x);
	BN_CTX_free(ctx);

	return ver;
}


/* Out: bytes_B, len_B.
*
* On failure, bytes_B will be set to NULL and len_B will be set to 0
*/
void  srp_generate_public_key(SRPVerifier * ver, const unsigned char ** bytes_B, int * len_B)
{
	BIGNUM             *B = BN_new();	
	BIGNUM             *k = 0;
	BIGNUM             *tmp1 = BN_new();
	BIGNUM             *tmp2 = BN_new();
	BN_CTX             *ctx = BN_CTX_new();
	
	*len_B = 0;
	*bytes_B = 0;
	ver->b = BN_new();

	if (!B || ver->b || !tmp1 || !tmp2 || !ctx)
		goto cleanup_and_exit;
	
	BN_rand(ver->b, 256, -1, 0);
	k = H_nn(ver->hash_alg, ver->ng->N, ver->ng->g);

	/* B = kv + g^b */
	BN_mul(tmp1, k, ver->v, ctx);
	BN_mod_exp(tmp2, ver->ng->g, ver->b, ver->ng->N, ctx);
	BN_add(B, tmp1, tmp2);
		
	*len_B = BN_num_bytes(B);
	*bytes_B = (const unsigned char *)malloc(*len_B);

	if (!*bytes_B)
	{
		free((void*)ver->username);
		delete ver;
		ver = 0;
		*len_B = 0;
		goto cleanup_and_exit;
	}

	BN_bn2bin(B, (unsigned char *)*bytes_B);

	ver->bytes_B = *bytes_B;
	

cleanup_and_exit:
	if (k) BN_free(k);
	BN_free(B);	
	BN_free(tmp1);
	BN_free(tmp2);
	BN_CTX_free(ctx);

}



/* Out: bytes_B, len_B.
 * 
 * On failure, bytes_B will be set to NULL and len_B will be set to 0
 */
SRPVerifier *  srp_verifier_new( SRP_HashAlgorithm alg, SRP_NGType ng_type, const char * username,
                                        const unsigned char * bytes_s, int len_s, 
                                        const unsigned char * bytes_v, int len_v,
                                        const unsigned char * bytes_A, int len_A,
                                        const unsigned char ** bytes_B, int * len_B,
                                        const char * n_hex, const char * g_hex )
{
    BIGNUM             *s    = BN_bin2bn(bytes_s, len_s, NULL);
    BIGNUM             *v    = BN_bin2bn(bytes_v, len_v, NULL);
    BIGNUM             *A    = BN_bin2bn(bytes_A, len_A, NULL);
    BIGNUM             *u    = 0;
    BIGNUM             *B    = BN_new();
    BIGNUM             *S    = BN_new();
    BIGNUM             *b    = BN_new();
    BIGNUM             *k    = 0;
    BIGNUM             *tmp1 = BN_new();
    BIGNUM             *tmp2 = BN_new();
    BN_CTX             *ctx  = BN_CTX_new();
    int                 ulen = strlen(username) + 1;
    NGConstant         *ng   = new_ng( ng_type, n_hex, g_hex );
    SRPVerifier *ver  = 0;

    *len_B   = 0;
    *bytes_B = 0;
    
    if( !s || !v || !A || !B || !S || !b || !tmp1 || !tmp2 || !ctx || !ng )
       goto cleanup_and_exit;
    
    ver = new SRPVerifier;

    if (!ver)
       goto cleanup_and_exit;

    init_random(); /* Only happens once */
    
    ver->username = (char *) malloc( ulen );
    ver->hash_alg = alg;
    ver->ng       = ng;

    if (!ver->username)
    {
       delete ver;
       ver = 0;
       goto cleanup_and_exit;
    }
    
    memcpy( (char*)ver->username, username, ulen );
    
    ver->authenticated = 0;
        
    /* SRP-6a safety check */
    BN_mod(tmp1, A, ng->N, ctx);
    if ( !BN_is_zero(tmp1) )
    {
       BN_rand(b, 256, -1, 0);
       
       k = H_nn(alg, ng->N, ng->g);
       
       /* B = kv + g^b */
       BN_mul(tmp1, k, v, ctx);
       BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
       BN_add(B, tmp1, tmp2);
       
       u = H_nn(alg, A, B);
       
       /* S = (A *(v^u)) ^ b */
       BN_mod_exp(tmp1, v, u, ng->N, ctx);
       BN_mul(tmp2, A, tmp1, ctx);
       BN_mod_exp(S, tmp2, b, ng->N, ctx);
       
       hash_num(alg, S, ver->session_key);
       
       calculate_M( alg, ng, ver->M, username, s, A, B, ver->session_key );
       calculate_H_AMK( alg, ver->H_AMK, A, ver->M, ver->session_key );
       
       *len_B   = BN_num_bytes(B);
	   *bytes_B = (const unsigned char *) malloc(*len_B);
       
       if( !*bytes_B )
       {
          free( (void*) ver->username );
          free( ver );
          ver = 0;
          *len_B = 0;
          goto cleanup_and_exit;
       }
       
       BN_bn2bin( B, (unsigned char *) *bytes_B );
          
       ver->bytes_B = *bytes_B;
    }
    
 cleanup_and_exit:
    BN_free(s);
    BN_free(v);
    BN_free(A);
    if (u) BN_free(u);
    if (k) BN_free(k);
    BN_free(B);
    BN_free(S);
    BN_free(b);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_CTX_free(ctx);
    
    return ver;
}

                                        


void srp_verifier_delete( SRPVerifier * ver )
{
   if (ver)
   {
      delete_ng( ver->ng );
	  BN_free(ver->b);
	  BN_free(ver->s);
      free( (char *) ver->username );
      free( (unsigned char *) ver->bytes_B );
	  memset(ver, 0, sizeof(*ver));
      free( ver );
   }
}



int srp_verifier_is_authenticated( SRPVerifier * ver )
{
    return ver->authenticated;
}


const char * srp_verifier_get_username( SRPVerifier * ver )
{
    return ver->username;
}


const unsigned char * srp_verifier_get_session_key( SRPVerifier * ver, int * key_length )
{
    if (key_length)
        *key_length = hash_length( ver->hash_alg );
    return ver->session_key;
}


int                   srp_verifier_get_session_key_length( SRPVerifier * ver )
{
    return hash_length( ver->hash_alg );
}


/* user_M must be exactly SHA512_DIGEST_LENGTH bytes in size */
void srp_verifier_verify_session( SRPVerifier * ver, const unsigned char * user_M, const unsigned char ** bytes_HAMK )
{
    if ( memcmp( ver->M, user_M, hash_length(ver->hash_alg) ) == 0 )
    {
        ver->authenticated = 1;
        *bytes_HAMK = ver->H_AMK;
    }
    else
        *bytes_HAMK = NULL;
}

/*******************************************************************************/

struct SRPUser * srp_user_new( SRP_HashAlgorithm alg, SRP_NGType ng_type, const char * username, 
                               const unsigned char * bytes_password, int len_password,
                               const char * n_hex, const char * g_hex )
{
    struct SRPUser  *usr  = (struct SRPUser *) malloc( sizeof(struct SRPUser) );
    int              ulen = strlen(username) + 1;

    if (!usr)
       goto err_exit;

    init_random(); /* Only happens once */
    
    usr->hash_alg = alg;
    usr->ng       = new_ng( ng_type, n_hex, g_hex );
    
    usr->a = BN_new();
    usr->A = BN_new();
    usr->S = BN_new();

    if (!usr->ng || !usr->a || !usr->A || !usr->S)
       goto err_exit;
    
    usr->username     = (const char *) malloc(ulen);
    usr->password     = (const unsigned char *) malloc(len_password);
    usr->password_len = len_password;

    if (!usr->username || !usr->password)
       goto err_exit;
    
    memcpy((char *)usr->username, username,       ulen);
    memcpy((char *)usr->password, bytes_password, len_password);

    usr->authenticated = 0;
    
    usr->bytes_A = 0;
    
    return usr;

 err_exit:
    if (usr)
    {
       BN_free(usr->a);
       BN_free(usr->A);
       BN_free(usr->S);
       if (usr->username)
          free((void*)usr->username);
       if (usr->password)
       {
          memset((void*)usr->password, 0, usr->password_len);
          free((void*)usr->password);
       }
       free(usr);
    }
    
    return 0;
}



void srp_user_delete( struct SRPUser * usr )
{
   if( usr )
   {
      BN_free( usr->a );
      BN_free( usr->A );
      BN_free( usr->S );
      
      delete_ng( usr->ng );

      memset((void*)usr->password, 0, usr->password_len);
      
      free((char *)usr->username);
      free((char *)usr->password);
      
      if (usr->bytes_A) 
         free( (char *)usr->bytes_A );

      memset(usr, 0, sizeof(*usr));
      free( usr );
   }
}



int srp_user_is_authenticated( struct SRPUser * usr)
{
    return usr->authenticated;
}


const char * srp_user_get_username( struct SRPUser * usr )
{
    return usr->username;
}



const unsigned char * srp_user_get_session_key( struct SRPUser * usr, int * key_length )
{
    if (key_length)
        *key_length = hash_length( usr->hash_alg );
    return usr->session_key;
}


int                   srp_user_get_session_key_length( struct SRPUser * usr )
{
    return hash_length( usr->hash_alg );
}



/* Output: username, bytes_A, len_A */
void  srp_user_start_authentication( struct SRPUser * usr, const char ** username, 
                                     const unsigned char ** bytes_A, int * len_A )
{
    BN_CTX  *ctx  = BN_CTX_new();
    
    BN_rand(usr->a, 256, -1, 0);
        
    BN_mod_exp(usr->A, usr->ng->g, usr->a, usr->ng->N, ctx);
        
    BN_CTX_free(ctx);
    
    *len_A   = BN_num_bytes(usr->A);
	*bytes_A = (const unsigned char *) malloc(*len_A);

    if (!*bytes_A)
    {
       *len_A = 0;
       *bytes_A = 0;
       *username = 0;
       return;
    }
        
    BN_bn2bin( usr->A, (unsigned char *) *bytes_A );
    
    usr->bytes_A = *bytes_A;
    *username = usr->username;
}


/* Output: bytes_M. Buffer length is SHA512_DIGEST_LENGTH */
void  srp_user_process_challenge( struct SRPUser * usr, 
                                  const unsigned char * bytes_s, int len_s, 
                                  const unsigned char * bytes_B, int len_B,
                                  const unsigned char ** bytes_M, int * len_M )
{
    BIGNUM *s    = BN_bin2bn(bytes_s, len_s, NULL);
    BIGNUM *B    = BN_bin2bn(bytes_B, len_B, NULL);
    BIGNUM *u    = 0;
    BIGNUM *x    = 0;
    BIGNUM *k    = 0;
    BIGNUM *v    = BN_new();
    BIGNUM *tmp1 = BN_new();
    BIGNUM *tmp2 = BN_new();
    BIGNUM *tmp3 = BN_new();
    BN_CTX *ctx  = BN_CTX_new();

    *len_M = 0;
    *bytes_M = 0;

    if( !s || !B || !v || !tmp1 || !tmp2 || !tmp3 || !ctx )
       goto cleanup_and_exit;
    
    u = H_nn(usr->hash_alg, usr->A, B);

    if (!u)
       goto cleanup_and_exit;
    
    x = calculate_x( usr->hash_alg, s, usr->username, usr->password, usr->password_len );

    if (!x)
       goto cleanup_and_exit;
    
    k = H_nn(usr->hash_alg, usr->ng->N, usr->ng->g);

    if (!k)
       goto cleanup_and_exit;
    
    /* SRP-6a safety check */
    if ( !BN_is_zero(B) && !BN_is_zero(u) )
    {
        BN_mod_exp(v, usr->ng->g, x, usr->ng->N, ctx);
        
        /* S = (B - k*(g^x)) ^ (a + ux) */
        BN_mul(tmp1, u, x, ctx);
        BN_add(tmp2, usr->a, tmp1);             /* tmp2 = (a + ux)      */
        BN_mod_exp(tmp1, usr->ng->g, x, usr->ng->N, ctx);
        BN_mul(tmp3, k, tmp1, ctx);             /* tmp3 = k*(g^x)       */
        BN_sub(tmp1, B, tmp3);                  /* tmp1 = (B - K*(g^x)) */
        BN_mod_exp(usr->S, tmp1, tmp2, usr->ng->N, ctx);

        hash_num(usr->hash_alg, usr->S, usr->session_key);
        
        calculate_M( usr->hash_alg, usr->ng, usr->M, usr->username, s, usr->A, B, usr->session_key );
        calculate_H_AMK( usr->hash_alg, usr->H_AMK, usr->A, usr->M, usr->session_key );
        
        *bytes_M = usr->M;
        if (len_M)
            *len_M = hash_length( usr->hash_alg );
    }
    else
    {
        *bytes_M = NULL;
        if (len_M) 
            *len_M   = 0;
    }

 cleanup_and_exit:
    
    BN_free(s);
    BN_free(B);
    BN_free(u);
    BN_free(x);
    BN_free(k);
    BN_free(v);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_free(tmp3);
    BN_CTX_free(ctx);
}
                                                  

void srp_user_verify_session( struct SRPUser * usr, const unsigned char * bytes_HAMK )
{
    if ( memcmp( usr->H_AMK, bytes_HAMK, hash_length(usr->hash_alg) ) == 0 )
        usr->authenticated = 1;
}

SRPVerifier::~SRPVerifier() {
	delete_ng(ng);
	BN_free(v);
	BN_free(b);
	BN_free(s);
	free((char *)username);
	free((unsigned char *)bytes_B);	
}