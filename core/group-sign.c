#include "group-sign.h"
#ifdef __cplusplus // workaround to allow using the library from C++
#define C99
#endif
#include "curve-specific.h"
#ifdef __cplusplus
#undef C99
#endif

// debug option: logs all state transitions
#define VERBOSE_LOGGING 0

#if VERBOSE_LOGGING
#include <stdio.h>
#endif

#ifndef HASH_TYPE
#error "HASH_TYPE is not defined. Make sure used curve is supported."
#endif

#define ECPSIZE ((2*MODBYTES) + 1)

// Attention: it used to be 4*MODBYTES, but now ECP2_toOctet will
// write (4*MODBYTES)+1 bytes. For backward compatibility reasons,
// ECP2_toOctet_compatibility and ECP2_fromOctet_compatibility can
// be used.
//
// Background:
//
// Recent changes in Milagro changed ECP2_ZZZ_toOctet.
// (The change came when they moved repositories, so you have to
// compare miracl/amcl with miracl/core.
//
// The new code appends one byte for the type now.
// In our case, we are using a non-compressed representation, i.e.,
// the value of the leading bit will be implicitely known.
//
// To guarantee backward compatibility with existing keys and old
// clients, let's stay on the old representation.
#define ECP2SIZE (4*MODBYTES)
#define BIGSIZE MODBYTES

// Output must be at least MODBYTES
void myhash(char *data, int len, char *output) {
  // HASH_TYPE is also number of output bytes
  //   SHA256 32 /**< SHA-256 hashing */
  //   SHA384 48 /**< SHA-384 hashing */
  //   SHA512 64 /**< SHA-512 hashing */
  octet msg = {len, len, data};
  octet out = {0, MODBYTES, output};
  GPhash(MC_SHA2, HASH_TYPE, &out, HASH_TYPE, &msg, -1, NULL);
}

struct GroupPublicKey {
    ECP2 X; // G2 ** x
    ECP2 Y; // G2 ** y

    // ZK of discrete-log knowledge for X and Y
    BIG cx;
    BIG sx;
    BIG cy;
    BIG sy;
};

struct GroupPrivateKey {
    struct GroupPublicKey pub;
    BIG x;
    BIG y;
};

struct JoinMessage {
    ECP Q; // G1 ** gsk

    BIG c;
    BIG s;
};

struct UserCredentials {
    ECP A;
    ECP B;
    ECP C;
    ECP D;
};

struct UserPrivateKey {
    struct UserCredentials cred;
    BIG gsk;
};

struct JoinResponse {
    struct UserCredentials cred;
    BIG c;
    BIG s;
};

struct Signature {
    ECP A;
    ECP B;
    ECP C;
    ECP D;
    ECP NYM;

    BIG c;
    BIG s;
};

// "bindings" -> these are the external interface

enum StateFlags {
  GS_SEEDED,
  GS_GROUP_PRIVKEY,
  GS_GROUP_PUBKEY,
  GS_USERCREDS,
};

typedef struct {
  csprng _rng;
  struct GroupPrivateKey _priv;
  struct UserPrivateKey _userPriv;
  int state;
} GS_State;

static void message(const char* msg)
{
  #if VERBOSE_LOGGING
  printf("%s\n", msg);
  #endif
}

static void log_state(int state)
{
  #if VERBOSE_LOGGING
  const char* flag_GS_SEEDED = "";
  if(state & (1 << GS_SEEDED))
  {
    flag_GS_SEEDED = "GS_SEEDED";
  }

  const char* flag_GS_GROUP_PRIVKEY = "";
  if(state & (1 << GS_GROUP_PRIVKEY))
  {
    flag_GS_GROUP_PRIVKEY = "GS_GROUP_PRIVKEY";
  }

  const char* flag_GS_GROUP_PUBKEY = "";
  if(state & (1 << GS_GROUP_PUBKEY))
  {
    flag_GS_GROUP_PUBKEY = "GS_GROUP_PUBKEY";
  }

  const char* flag_GS_USERCREDS = "";
  if(state & (1 << GS_USERCREDS))
  {
    flag_GS_USERCREDS = "GS_USERCREDS";
  }

  printf("state changed to %d (%s %s %s %s)\n",
         state,
         flag_GS_SEEDED,
         flag_GS_GROUP_PRIVKEY,
         flag_GS_GROUP_PUBKEY,
         flag_GS_USERCREDS);
  #endif
}

static void PAIR_normalized_triple_ate(FP12 *r, ECP2 *P, ECP *Q, ECP2 *R, ECP *S, ECP2 *T, ECP *U)
{
  // Use new multi-pairing mechanism
  FP12 rr[ATE_BITS];
  PAIR_initmp(rr);
  PAIR_another(rr, P, Q); // This already normalizes with ECP*_affine
  PAIR_another(rr, R, S);
  PAIR_another(rr, T, U);
  PAIR_miller(r, rr);
  PAIR_fexp(r);
}

static int serialize_BIG(BIG* in, octet* out)
{
  int len = out->len;
  out->len += BIGSIZE;
  if (out->len <= out->max) {
    BIG_toBytes(&out->val[len], *in);
    return 1;
  }
  return 0;
}
static int deserialize_BIG(octet* in, BIG* out)
{
  int len = in->len;
  in->len += BIGSIZE;
  if (in->len <= in->max) {
    BIG_fromBytes(*out, &in->val[len]);
    return 1;
  }
  return 0;
}

// Similar to ECP2_ZZZ_toOctet, but assumes no-compression,
// and thus does not insert a leading byte with that value.
//
// To preserve binary compatibility with older clients and
// with already published keys, let's keep compatibility.
static void ECP2_toOctet_compatibility(octet *W, ECP2 *Q)
{
  BIG b;
  FP2 qx, qy;
  ECP2_get(&qx, &qy, Q);

  FP_redc(b, &(qx.a));
  BIG_toBytes(&(W->val[0]), b);
  FP_redc(b, &(qx.b));
  BIG_toBytes(&(W->val[MODBYTES]), b);

  FP_redc(b, &(qy.a));
  BIG_toBytes(&(W->val[2 * MODBYTES]), b);
  FP_redc(b, &(qy.b));
  BIG_toBytes(&(W->val[3 * MODBYTES]), b);

  W->len = 4 * MODBYTES;
}

// similar to ECP2_ZZZ_fromOctet, but assumes no-compression,
// and thus does not expect a leading byte with that value.
//
// To preserve binary compatibility with older clients and
// with already published keys, let's keep compatibility.
int ECP2_fromOctet_compatibility(ECP2 *Q, octet *W)
{
  BIG b;
  FP2 qx, qy;

  BIG_fromBytes(b, &(W->val[0]));
  FP_nres(&(qx.a), b);
  BIG_fromBytes(b, &(W->val[MODBYTES]));
  FP_nres(&(qx.b), b);

  BIG_fromBytes(b, &(W->val[2 * MODBYTES]));
  FP_nres(&(qy.a), b);
  BIG_fromBytes(b, &(W->val[3 * MODBYTES]));
  FP_nres(&(qy.b), b);

  if (ECP2_set(Q, &qx, &qy))
    return 1;

  return 0;
}

static int serialize_ECP2(ECP2* in, octet* out)
{
  int len = out->len;
  out->len += ECP2SIZE;
  if (out->len <= out->max) {
    octet tmp = {0, out->max - len, &out->val[len]};
    ECP2_toOctet_compatibility(&tmp, in);
    return 1;
  }
  return 0;
}
static int deserialize_ECP2(octet* in, ECP2* out)
{
  int len = in->len;
  in->len += ECP2SIZE;
  if (in->len <= in->max) {
    octet tmp = {0, in->max - len, &in->val[len]};
    return ECP2_fromOctet_compatibility(out, &tmp) && 1;
  }
  return 0;
}

static int serialize_ECP(ECP* in, octet* out)
{
  int len = out->len;
  out->len += ECPSIZE;
  if (out->len <= out->max) {
    octet tmp = {0, out->max - len, &out->val[len]};
    ECP_toOctet(&tmp, in, false);
    return 1;
  }
  return 0;
}
static int deserialize_ECP(octet* in, ECP* out)
{
  int len = in->len;
  in->len += ECPSIZE;
  if (in->len <= in->max) {
    octet tmp = {0, in->max - len, &in->val[len]};
    return ECP_fromOctet(out, &tmp) && 1;
  }
  return 0;
}

/**
 * ECP_ZZZ_mapit changed. For backward compatibility reasons,
 * we have to use the old implementation. Otherwise, old
 * clients (or old group keys) will stop working.
 *
 * The folling code is a simplified version of the original
 * ECP_ZZZ_mapit implementation.
 */
void ECP_mapit_compatibility(ECP *P,octet *W)
{
  BIG q,x;
  BIG_fromBytes(x,W->val);
  BIG_rcopy(q,Modulus);
  BIG_mod(x,q);

  for (;;) {
    for (;;) {
        ECP_setx(P,x,0); // for CURVETYPE_ZZZ!=MONTGOMERY
        BIG_inc(x,1);
        BIG_norm(x);
        if (!ECP_isinf(P))
          break;
    }
    ECP_cfp(P);
    if (!ECP_isinf(P))
      break;
  }
}

static void mapit(char *h, ECP *P)
{
    octet o = {MODBYTES, MODBYTES, h};
    ECP_mapit_compatibility(P, &o);
}

static void setG1(ECP* X)
{
    BIG x, y;
    BIG_rcopy(x, CURVE_Gx);
    BIG_rcopy(y, CURVE_Gy);
    ECP_set(X, x, y);
}

static void setG2(ECP2* X)
{
    FP2 wx,wy;
    FP_rcopy(&(wx.a),CURVE_Pxa);
    FP_rcopy(&(wx.b),CURVE_Pxb);
    FP_rcopy(&(wy.a),CURVE_Pya);
    FP_rcopy(&(wy.b),CURVE_Pyb);
    ECP2_set(X,&wx,&wy);
}

static void randomModOrder(BIG x, csprng *RNG)
{
    BIG order;
    BIG_rcopy(order, CURVE_Order);
    BIG_randomnum(x, order, RNG);
}

static void ECP2challenge(ECP2* Y, ECP2* G, ECP2* GR, BIG c)
{
    char hh[MODBYTES];
    char tmp[3 * ECP2SIZE];
    octet TMP = {0, sizeof(tmp), tmp};

    serialize_ECP2(Y, &TMP);
    serialize_ECP2(G, &TMP);
    serialize_ECP2(GR, &TMP);
    myhash(tmp, TMP.len, hh);

    BIG_fromBytes(c, hh);
    BIG order;
    BIG_rcopy(order, CURVE_Order);
    BIG_mod(c, order);
}

// message is either NULL or has length MODBYTES
// If !message c = H(Y|G|GR)
// If message c = H(m|Y|G|GR)
static void ECPchallenge(char* message, ECP* Y, ECP* G, ECP* GR, BIG c)
{
    char hh[MODBYTES];
    char tmp[MODBYTES + 3 * ECPSIZE];
    octet TMP = {0, sizeof(tmp), tmp};
    if (message) {
        for (int i = 0; i < MODBYTES; ++i) {
            tmp[i] = message[i];
        }
        TMP.len = MODBYTES;
    }
    serialize_ECP(Y, &TMP);
    serialize_ECP(G, &TMP);
    serialize_ECP(GR, &TMP);
    myhash(tmp, TMP.len, hh);

    BIG_fromBytes(c, hh);
    BIG order;
    BIG_rcopy(order, CURVE_Order);
    BIG_mod(c, order);
}

// make POK of X such that Y = G ** X
// v is a random integer mod group order
// output is T, r
static void makeECPProof(csprng* RNG, ECP* G, ECP* Y, BIG x, char *message, BIG c, BIG s)
{
    BIG r, order;
    BIG_rcopy(order, CURVE_Order);
    randomModOrder(r, RNG);
    ECP GR;
    ECP_copy(&GR, G);
    PAIR_G1mul(&GR, r);
    ECPchallenge(message, Y, G, &GR, c);
    BIG_modmul(s, c, x, order);
    BIG_add(s, s, r);
    BIG_mod(s, order);
}

static void ECPchallengeEquals(char* message, ECP* Y, ECP* Z, ECP* A, ECP* B, ECP* AR, ECP* BR, BIG c)
{
    char hh[MODBYTES];
    char tmp[MODBYTES + 6 * ECPSIZE];
    octet TMP = {0, sizeof(tmp), tmp};
    if (message) {
        for (int i = 0; i < MODBYTES; ++i) {
            tmp[i] = message[i];
        }
        TMP.len = MODBYTES;
    }
    serialize_ECP(Y, &TMP);
    serialize_ECP(Z, &TMP);
    serialize_ECP(A, &TMP);
    serialize_ECP(B, &TMP);
    serialize_ECP(AR, &TMP);
    serialize_ECP(BR, &TMP);
    myhash(tmp, TMP.len, hh);

    BIG_fromBytes(c, hh);
    BIG order;
    BIG_rcopy(order, CURVE_Order);
    BIG_mod(c, order);
}

static void makeECPProofEquals(csprng* RNG, ECP* A, ECP* B, ECP* Y, ECP* Z, BIG x, char* message, BIG c, BIG s)
{
    BIG r, order;
    BIG_rcopy(order, CURVE_Order);
    randomModOrder(r, RNG);
    ECP AR, BR;
    ECP_copy(&AR, A);
    ECP_copy(&BR, B);
    PAIR_G1mul(&AR, r);
    PAIR_G1mul(&BR, r);
    ECPchallengeEquals(message, Y, Z, A, B, &AR, &BR, c);
    BIG_modmul(s, c, x, order);
    BIG_add(s, s, r);
    BIG_mod(s, order);
}

// POK of X such that Y = G ** X
// verify that T = (G ** R) * (Y ** C), C = H(G, Y, T)
static int verifyECPProof(ECP* G, ECP* Y, char* message, BIG c, BIG s)
{
    BIG cn, order;
    BIG_rcopy(order, CURVE_Order);
    BIG_modneg(cn, c, order);
    ECP GS, YC;
    ECP_copy(&GS, G);
    ECP_copy(&YC, Y);
    PAIR_G1mul(&GS, s);
    PAIR_G1mul(&YC, cn);
    ECP_add(&GS, &YC);
    BIG cc;
    ECPchallenge(message, Y, G, &GS, cc);
    return BIG_comp(c, cc) == 0;
}

static int verifyECPProofEquals(ECP* A, ECP* B, ECP* Y, ECP* Z, char* message, BIG c, BIG s)
{
    BIG cn, order;
    BIG_rcopy(order, CURVE_Order);
    BIG_modneg(cn, c, order);
    ECP AS, YC, BS, ZC;
    ECP_copy(&AS, A);
    ECP_copy(&YC, Y);
    ECP_copy(&BS, B);
    ECP_copy(&ZC, Z);
    PAIR_G1mul(&AS, s);
    PAIR_G1mul(&YC, cn);
    PAIR_G1mul(&BS, s);
    PAIR_G1mul(&ZC, cn);
    ECP_add(&AS, &YC);
    ECP_add(&BS, &ZC);
    BIG cc;
    ECPchallengeEquals(message, Y, Z, A, B, &AS, &BS, cc);
    return BIG_comp(c, cc) == 0;
}

// make POK of X such that Y = G ** X
// output is c, s
static void makeECP2Proof(csprng* RNG, ECP2* G, ECP2* Y, BIG x, BIG c, BIG s)
{
    BIG r, order;
    BIG_rcopy(order, CURVE_Order);
    randomModOrder(r, RNG);
    ECP2 GR;
    ECP2_copy(&GR, G);
    PAIR_G2mul(&GR, r);
    ECP2challenge(Y, G, &GR, c);
    BIG_modmul(s, c, x, order);
    BIG_add(s, s, r);
    BIG_mod(s, order);
}


// POK of X such that Y = G ** X
// verify that T = (G ** R) * (Y ** C), C = H(G, Y, T)
static int verifyECP2Proof(ECP2* G, ECP2* Y, BIG c, BIG s)
{
    BIG cn, order;
    BIG_rcopy(order, CURVE_Order);
    BIG_modneg(cn, c, order);
    ECP2 GS, YC;
    ECP2_copy(&GS, G);
    ECP2_copy(&YC, Y);
    PAIR_G2mul(&GS, s);
    PAIR_G2mul(&YC, cn);
    ECP2_add(&GS, &YC);
    BIG cc;
    ECP2challenge(Y, G, &GS, cc);
    return BIG_comp(c, cc) == 0;
}

// Note for future optimizations: In the implemented scheme batch signature verification
// (for different signers) seems not possible. However, https://eprint.iacr.org/2007/172.pdf
// suggests a modified scheme (with some restrictions) where fast batch signature verification
// is possible. If those restrictions are acceptable for our use case, then this would mean big
// improvements on server-side verification time (currently around 4ms per signature per core on EC2 c5
// instance).
  
// We currently apply a couple of optimizations that are possible in the current scheme:     

// According to https://eprint.iacr.org/2009/598.pdf we can transform
// the original check e(A, Y) =? e(B, G2) and e(A + D, X) =? e(C, G2)
// into a product of four pairings:
//
// e(e1·A, Y)·
// e(-e1·B, G2)·
// e(e2·(A + D), X)·
// e(-e2·C, G2) == 1?
//
// Then, according to https://eprint.iacr.org/2014/401.pdf
// we can also factor out pairings sharing a common element, leading to
// the following product of three pairings:
//
// e(e1·A, Y)·
// e((-e1·B) + (-e2·C), G2)·
// e(e2·(A + D), X) == 1?
//
static int verifyAuxFast(ECP* A, ECP* B, ECP* C, ECP* D, ECP2* X, ECP2 *Y, csprng *RNG) {
  ECP AA, BB, CC;
  ECP2 G2;
  BIG e1, e2, ne1, ne2, order;
  FP12 w, y;

  // A != 1
  if (ECP_isinf(A)) {
      return 0;
  }

  BIG_rcopy(order, CURVE_Order);
  setG2(&G2);

  // These factors can be half the bits of the group order, but this is
  // because of efficiency. Not sure if this makes a difference with milagro-crypto-c, would
  // need to test.
  randomModOrder(e1, RNG);
  randomModOrder(e2, RNG);
  BIG_modneg(ne1, e1, order);
  BIG_modneg(ne2, e2, order);

  // AA = e1·A
  ECP_copy(&AA, A);
  PAIR_G1mul(&AA, e1);

  // BB = -e1·B
  ECP_copy(&BB, B);
  PAIR_G1mul(&BB, ne1);

  // CC = -e2·C
  ECP_copy(&CC, C);
  PAIR_G1mul(&CC, ne2);

  // BB = (-e1·B) + (-e2·C)
  ECP_add(&BB, &CC);

  // CC = e2·(A + D)
  ECP_copy(&CC, A);
  ECP_add(&CC, D);
  PAIR_G1mul(&CC, e2);

  // w = e(e1·A, Y)·e((-e1·B) + (-e2·C), G2)·e(e2·(A + D), X)
  PAIR_normalized_triple_ate(&w, Y, &AA, &G2, &BB, X, &CC);

  FP12_one(&y);

  if (!FP12_equals(&w, &y)) {
      return 0;
  }

  return 1;
}

static int serialize_group_public_key(struct GroupPublicKey* in, octet* out)
{
  return
  serialize_ECP2(&in->X, out) &&
  serialize_ECP2(&in->Y, out) &&
  serialize_BIG(&in->cx, out) &&
  serialize_BIG(&in->sx, out) &&
  serialize_BIG(&in->cy, out) &&
  serialize_BIG(&in->sy, out);
}

static int verifyGroupPublicKey(struct GroupPublicKey *pub)
{
    ECP2 W;
    setG2(&W);

    return verifyECP2Proof(&W, &pub->X, pub->cx, pub->sx)
        && verifyECP2Proof(&W, &pub->Y, pub->cy, pub->sy);
}

static int deserialize_group_public_key(octet* in, struct GroupPublicKey* out)
{
  return
  deserialize_ECP2(in, &out->X) &&
  deserialize_ECP2(in, &out->Y) &&
  deserialize_BIG(in, &out->cx) &&
  deserialize_BIG(in, &out->sx) &&
  deserialize_BIG(in, &out->cy) &&
  deserialize_BIG(in, &out->sy) &&
  verifyGroupPublicKey(out); // TODO: should this be done here?
}

static int serialize_group_private_key(struct GroupPrivateKey* in, octet* out)
{
  return serialize_group_public_key(&in->pub, out) &&
  serialize_BIG(&in->x, out) &&
  serialize_BIG(&in->y, out);
}

static int _checkPrivateKey(struct GroupPrivateKey* key)
{
  ECP2 X, Y;
  setG2(&X);
  setG2(&Y);
  PAIR_G2mul(&X, key->x);
  PAIR_G2mul(&Y, key->y);
  return ECP2_equals(&X, &key->pub.X) && ECP2_equals(&Y, &key->pub.Y);
}

static int deserialize_group_private_key(octet* in, struct GroupPrivateKey* out)
{
  return deserialize_group_public_key(in, &out->pub) &&
  deserialize_BIG(in, &out->x) &&
  deserialize_BIG(in, &out->y) &&
  _checkPrivateKey(out); // TODO: should this be done here?
}

static int serialize_join_message(struct JoinMessage* in, octet* out)
{
  return
  serialize_ECP(&in->Q, out) &&
  serialize_BIG(&in->c, out) &&
  serialize_BIG(&in->s, out);
}

static int deserialize_join_message(octet* in, struct JoinMessage* out)
{
  return
  deserialize_ECP(in, &out->Q) &&
  deserialize_BIG(in, &out->c) &&
  deserialize_BIG(in, &out->s);
}

static int serialize_user_credentials(struct UserCredentials* in, octet* out)
{
  return
  serialize_ECP(&in->A, out) &&
  serialize_ECP(&in->B, out) &&
  serialize_ECP(&in->C, out) &&
  serialize_ECP(&in->D, out);
}

static int deserialize_user_credentials(octet* in, struct UserCredentials* out)
{
  return
  deserialize_ECP(in, &out->A) &&
  deserialize_ECP(in, &out->B) &&
  deserialize_ECP(in, &out->C) &&
  deserialize_ECP(in, &out->D);
}

static int serialize_join_response(struct JoinResponse* in, octet* out)
{
  return
  serialize_user_credentials(&in->cred, out) &&
  serialize_BIG(&in->c, out) &&
  serialize_BIG(&in->s, out);
}

static int deserialize_join_response(octet* in, struct JoinResponse* out)
{
  return
  deserialize_user_credentials(in, &out->cred) &&
  deserialize_BIG(in, &out->c) &&
  deserialize_BIG(in, &out->s);
}

static int serialize_user_private_key(struct UserPrivateKey* in, octet* out)
{
  return
  serialize_user_credentials(&in->cred, out) &&
  serialize_BIG(&in->gsk, out);
}

static int deserialize_user_private_key(octet* in, struct UserPrivateKey* out)
{
  return
  deserialize_user_credentials(in, &out->cred) &&
  deserialize_BIG(in, &out->gsk);
}

static int serialize_signature(struct Signature* in, octet* out)
{
  return
  serialize_ECP(&in->A, out) &&
  serialize_ECP(&in->B, out) &&
  serialize_ECP(&in->C, out) &&
  serialize_ECP(&in->D, out) &&
  serialize_ECP(&in->NYM, out) &&
  serialize_BIG(&in->c, out) &&
  serialize_BIG(&in->s, out);
}

static int deserialize_signature(octet* in, struct Signature* out)
{
  return
  deserialize_ECP(in, &out->A) &&
  deserialize_ECP(in, &out->B) &&
  deserialize_ECP(in, &out->C) &&
  deserialize_ECP(in, &out->D) &&
  deserialize_ECP(in, &out->NYM) &&
  deserialize_BIG(in, &out->c) &&
  deserialize_BIG(in, &out->s);
}

static int serialize_signature_tag(struct Signature* in, octet* out)
{
  return
  serialize_ECP(&in->NYM, out);
}

static void join_client(csprng *RNG, char* challenge, int challenge_len, struct JoinMessage *j, struct UserPrivateKey *priv)
    // BIG gsk, ECP* Q, ECP* T, BIG rr) // output
{
  message("join_client");
    ECP G;
    setG1(&G);

    ECP_copy(&j->Q, &G);
    randomModOrder(priv->gsk, RNG);
    PAIR_G1mul(&j->Q, priv->gsk);

    char h[MODBYTES];
    myhash(challenge, challenge_len, h);
    makeECPProof(RNG, &G, &j->Q, priv->gsk, h, j->c, j->s);

  message("join_client: done");
}

static int join_server(csprng *RNG, struct GroupPrivateKey *priv, struct JoinMessage *j, char* challenge, int challenge_len, struct JoinResponse *resp)
{
    BIG order;
    BIG_rcopy(order, CURVE_Order);

    ECP G;
    setG1(&G);

    char h[MODBYTES];
    myhash(challenge, challenge_len, h);

    int ok = verifyECPProof(&G, &j->Q, h, j->c, j->s);
    if (ok) {
        ECP *A = &resp->cred.A;
        ECP *B = &resp->cred.B;
        ECP *C = &resp->cred.C;
        ECP *D = &resp->cred.D;
        ECP *Q = &j->Q;

        BIG r;
        randomModOrder(r, RNG);
        ECP_copy(A, &G);
        PAIR_G1mul(A, r);

        ECP_copy(B, A);
        PAIR_G1mul(B, priv->y);

        ECP_copy(D, Q);
        BIG tmp;
        BIG_modmul(tmp, r, priv->y, order);
        PAIR_G1mul(D, tmp);

        ECP_copy(C, A);
        ECP_add(C, D);
        PAIR_G1mul(C, priv->x);

        makeECPProofEquals(RNG, &G, Q, B, D, tmp, 0, resp->c, resp->s);
    }
    return ok;
}

static int setup(csprng *RNG, struct GroupPrivateKey *priv)
{
    ECP2 W;
    setG2(&W);

    ECP2_copy(&priv->pub.X,&W);
    ECP2_copy(&priv->pub.Y,&W);

    // Choose random x,y less than the group order
    randomModOrder(priv->x, RNG);
    randomModOrder(priv->y, RNG);

    // Compute public keys
    PAIR_G2mul(&priv->pub.X, priv->x);
    PAIR_G2mul(&priv->pub.Y, priv->y);

    makeECP2Proof(RNG, &W, &priv->pub.X, priv->x, priv->pub.cx, priv->pub.sx);
    makeECP2Proof(RNG, &W, &priv->pub.Y, priv->y, priv->pub.cy, priv->pub.sy);

    return 0;
}

static int join_finish_client(struct GroupPublicKey *pub, struct UserPrivateKey *priv, struct JoinResponse *resp, csprng *RNG)
{
    ECP G, Q;
    setG1(&G);

    ECP_copy(&Q, &G);
    PAIR_G1mul(&Q, priv->gsk);

    if (!verifyECPProofEquals(&G, &Q, &resp->cred.B, &resp->cred.D, 0, resp->c, resp->s)) {
        return 0;
    }

    if (!verifyAuxFast(&resp->cred.A, &resp->cred.B, &resp->cred.C, &resp->cred.D, &pub->X, &pub->Y, RNG)) {
        return 0;
    }

    ECP_copy(&priv->cred.A, &resp->cred.A);
    ECP_copy(&priv->cred.B, &resp->cred.B);
    ECP_copy(&priv->cred.C, &resp->cred.C);
    ECP_copy(&priv->cred.D, &resp->cred.D);

    return 1;
}

static void sign(csprng *RNG, struct UserPrivateKey *priv, char* msg, int msg_len, char* bsn, int bsn_len, struct Signature *sig)
{
    char hh[2 * MODBYTES];
    char h[MODBYTES];

    ECP_copy(&sig->A, &priv->cred.A);
    ECP_copy(&sig->B, &priv->cred.B);
    ECP_copy(&sig->C, &priv->cred.C);
    ECP_copy(&sig->D, &priv->cred.D);

    // Randomize credentials for signature
    BIG r;
    randomModOrder(r, RNG);
    PAIR_G1mul(&sig->A, r);
    PAIR_G1mul(&sig->B, r);
    PAIR_G1mul(&sig->C, r);
    PAIR_G1mul(&sig->D, r);

    // Map basename to point in G1
    ECP BSN;
    myhash(bsn, bsn_len, h);
    mapit(h, &BSN);
    ECP_copy(&sig->NYM, &BSN);
    PAIR_G1mul(&sig->NYM, priv->gsk);

    // Compute H(H(msg) || H(bsn)) to be used in proof of equality
    myhash(msg, msg_len, &hh[0]);
    myhash(bsn, bsn_len, &hh[MODBYTES]);
    myhash(hh, sizeof(hh), h);
    makeECPProofEquals(RNG, &sig->B, &BSN, &sig->D, &sig->NYM, priv->gsk, h, sig->c, sig->s);
}

static int verify(char *msg, int msg_len, char *bsn, int bsn_len, struct Signature *sig, struct GroupPublicKey *pub, csprng *RNG)
{
    char hh[2 * MODBYTES];
    char h[MODBYTES];

    // Map basename to point in G1
    ECP BSN;
    myhash(bsn, bsn_len, h);
    mapit(h, &BSN);

    // Compute H(H(msg) || H(bsn)) to be used in proof of equality
    myhash(msg, msg_len, &hh[0]);
    myhash(bsn, bsn_len, &hh[MODBYTES]);
    myhash(hh, sizeof(hh), h);

    return verifyECPProofEquals(&sig->B, &BSN, &sig->D, &sig->NYM, h, sig->c, sig->s)
     && !ECP_isinf(&sig->A) && !ECP_isinf(&sig->B)
     && verifyAuxFast(&sig->A, &sig->B, &sig->C, &sig->D, &pub->X, &pub->Y, RNG);
}

// External interface:


// Start - Operations that modify internal state
void GS_initState(void* rawstate) {
  GS_State* state = (GS_State*)rawstate;
  state->state = 0;
  log_state(state->state);
}

int GS_seed(void* rawstate, char* seed, int seed_length) {
  GS_State* state = (GS_State*)rawstate;
  if (seed_length < 128) {
    return GS_SEED_TOO_SMALL;
  }
  RAND_seed(&state->_rng, seed_length, seed);
  state->state |= 1 << GS_SEEDED;
  log_state(state->state);
  return GS_RETURN_SUCCESS;
}

int GS_setupGroup(void* rawstate) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_SEEDED)&(state->state))) {
    return GS_NOT_SEEDED;
  }
  state->state &= (1 << GS_SEEDED);
  setup(&state->_rng, &state->_priv);
  state->state |= 1 << GS_GROUP_PRIVKEY;
  state->state |= 1 << GS_GROUP_PUBKEY;
  log_state(state->state);
  return GS_RETURN_SUCCESS;
}

int GS_loadGroupPrivKey(void* rawstate, char* data, int len) {
  GS_State* state = (GS_State*)rawstate;
  state->state &= (1 << GS_SEEDED);
  octet o = {0, len, data};
  if (!deserialize_group_private_key(&o, &state->_priv)) {
    return GS_INVALID_GROUP_PRIVATE_KEY;
  }
  state->state |= 1 << GS_GROUP_PRIVKEY;
  state->state |= 1 << GS_GROUP_PUBKEY;
  log_state(state->state);
  return GS_RETURN_SUCCESS;
}

int GS_loadGroupPubKey(void* rawstate, char* data, int len) {
  GS_State* state = (GS_State*)rawstate;
  state->state &= (1 << GS_SEEDED);
  octet o = {0, len, data};
  if (!deserialize_group_public_key(&o, &state->_priv.pub)) {
    return GS_INVALID_GROUP_PUBLIC_KEY;
  }
  state->state |= 1 << GS_GROUP_PUBKEY;
  log_state(state->state);
  return GS_RETURN_SUCCESS;
}

int GS_startJoin(
  void* rawstate,
  char* challenge, // in
  int challenge_len, // in
  char* gsk, int* len_gsk, // out
  char* joinmsg, int* len // out
) {
  message("GS_startJoin called");

  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_SEEDED)&state->state)) {
    message("GS_startJoin: GS_NOT_SEEDED");
    return GS_NOT_SEEDED;
  }

  struct JoinMessage j;
  struct UserPrivateKey userPriv;
  join_client(&state->_rng, challenge, challenge_len, &j, &userPriv);
  octet o = {0, *len, joinmsg};
  if (!serialize_join_message(&j, &o)) {
    message("GS_startJoin: GS_OUTPUT_BUFFER_TOO_SMALL");
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *len = o.len;

  octet o2 = {0, *len_gsk, gsk};
  if (!serialize_BIG(&userPriv.gsk, &o2)) {
    message("GS_startJoin: GS_OUTPUT_BUFFER_TOO_SMALL");
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *len_gsk = o2.len;

  message("GS_startJoin: GS_RETURN_SUCCESS");
  return GS_RETURN_SUCCESS;
}


int GS_finishJoin(
  char* publickey, int len_publickey, // in
  char* gsk, int len_gsk, // in
  char* joinresponse, int len, // in
  char* credentials, int* len_credentials // out
) {
  struct GroupPublicKey pub;
  octet op = {0, len_publickey, publickey};
  if (!deserialize_group_public_key(&op, &pub)) {
    return GS_INVALID_GROUP_PUBLIC_KEY;
  }

  octet og = {0, len_gsk, gsk};
  struct UserPrivateKey priv;
  if (!deserialize_BIG(&og, &priv.gsk)) {
    return GS_INVALID_USER_PRIVATE_KEY;
  }

  // Temporal random number generator for fast verification, seeded with user secret.
  csprng rng;
  RAND_seed(&rng, len_gsk, gsk);

  octet o = {0, len, joinresponse};
  struct JoinResponse resp;
  if (!deserialize_join_response(&o, &resp) || !join_finish_client(&pub, &priv, &resp, &rng)) {
    return GS_INVALID_JOIN_RESPONSE;
  }

  octet oc = {0, *len_credentials, credentials};
  if (!serialize_user_private_key(&priv, &oc)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *len_credentials = oc.len;

  return GS_RETURN_SUCCESS;
}

int GS_loadUserCredentials(void* rawstate, char* in, int in_len) {
  GS_State* state = (GS_State*)rawstate;
  state->state &= ~(1 << GS_USERCREDS);
  log_state(state->state);

  octet o = {0, in_len, in};
  if (!deserialize_user_private_key(&o, &state->_userPriv)) {
    return GS_INVALID_USER_CREDENTIALS;
  }

  state->state |= (1 << GS_USERCREDS);
  log_state(state->state);
  return GS_RETURN_SUCCESS;
}
// End - Operations that modify internal state

int GS_exportGroupPrivKey(void* rawstate, char* out, int* out_len) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_GROUP_PRIVKEY)&state->state)) {
    return GS_NOT_SET_GROUP_PRIVATE_KEY;
  }
  octet o = {0, *out_len, out};
  if (!serialize_group_private_key(&state->_priv, &o)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *out_len = o.len;
  return GS_RETURN_SUCCESS;
}

int GS_exportGroupPubKey(void* rawstate, char* out, int* out_len) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_GROUP_PUBKEY)&(state->state))) {
    return GS_NOT_SET_GROUP_PUBLIC_KEY;
  }
  octet o = {0, *out_len, out};
  if (!serialize_group_public_key(&state->_priv.pub, &o)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *out_len = o.len;
  return GS_RETURN_SUCCESS;
}

int GS_exportUserCredentials(void* rawstate, char* out, int* out_len) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_USERCREDS)&state->state)) {
    return GS_NOT_SET_USER_CREDENTIALS;
  }
  octet o = {0, *out_len, out};
  if (!serialize_user_private_key(&state->_userPriv, &o)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *out_len = o.len;
  return GS_RETURN_SUCCESS;
}

int GS_processJoin(void* rawstate, char* joinmsg, int joinmsg_len, char* challenge, int challenge_len, char* out, int* out_len) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_SEEDED)&state->state)) {
    message("GS_SEEDED not set");
    return GS_NOT_SEEDED;
  }
  if (!((1 << GS_GROUP_PRIVKEY)&state->state)) {
    message("GS_GROUP_PRIVKEY not set");
    return GS_NOT_SET_GROUP_PRIVATE_KEY;
  }

  struct JoinMessage join;
  struct JoinResponse resp;
  octet o = {0, joinmsg_len, joinmsg};
  octet oo = {0, *out_len, out};
  if (
    !deserialize_join_message(&o, &join) ||
    !join_server(&state->_rng, &state->_priv, &join, challenge, challenge_len, &resp)
  ) {
    return GS_INVALID_JOIN_MESSAGE;
  }

  if (!serialize_join_response(&resp, &oo)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }

  *out_len = oo.len;

  return GS_RETURN_SUCCESS;
}

int GS_sign(void* rawstate, char* msg, int msg_len, char* bsn, int bsn_len, char* signature, int* len) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_SEEDED)&state->state)) {
    message("GS_SEEDED not set");
    return GS_NOT_SEEDED;
  }
  if (!((1 << GS_USERCREDS)&state->state)) {
    return GS_NOT_SET_USER_CREDENTIALS;
  }
  struct Signature sig;
  sign(&state->_rng, &state->_userPriv, msg, msg_len, bsn, bsn_len, &sig);
  octet o = {0, *len, signature};
  if (!serialize_signature(&sig, &o)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *len = o.len;
  return GS_RETURN_SUCCESS;
}

int GS_verify(void* rawstate, char* msg, int msg_len, char* bsn, int bsn_len, char* signature, int len) {
  GS_State* state = (GS_State*)rawstate;
  if (!((1 << GS_GROUP_PUBKEY)&state->state)) {
    return GS_NOT_SET_GROUP_PUBLIC_KEY;
  }
  struct Signature sig;
  octet o = {0, len, signature};
  if (!deserialize_signature(&o, &sig)) {
    return GS_INVALID_SIGNATURE;
  }
  if (!verify(msg, msg_len, bsn, bsn_len, &sig, &state->_priv.pub, &state->_rng)) {
    return GS_RETURN_FAILURE;
  }
  return GS_RETURN_SUCCESS;
}

int GS_getSignatureTag(char* signature, int sig_len, char* tag, int* tag_len) {
  struct Signature sig;
  octet o = {0, sig_len, signature};
  octet oo = {0, *tag_len, tag};
  if (!deserialize_signature(&o, &sig)) {
    return GS_INVALID_SIGNATURE;
  }
  if (!serialize_signature_tag(&sig, &oo)) {
    return GS_OUTPUT_BUFFER_TOO_SMALL;
  }
  *tag_len = oo.len;
  return GS_RETURN_SUCCESS;
}

size_t GS_getStateSize() {
  return sizeof(GS_State);
}

const char* GS_version() {
  return "1.0";
}

const char* GS_curve() {
  return GS_CURVE;
}

int GS_success() {
  return GS_RETURN_SUCCESS;
}

int GS_failure() {
  return GS_RETURN_FAILURE;
}

const char* GS_error(int error_code) {
  switch (error_code) {
    case GS_RETURN_FAILURE: return "ko";
    case GS_RETURN_SUCCESS: return "ok";
    case GS_SEED_TOO_SMALL: return "seed too small";
    case GS_NOT_SEEDED: return "not seeded";
    case GS_INVALID_GROUP_PRIVATE_KEY: return "invalid group private key";
    case GS_INVALID_GROUP_PUBLIC_KEY: return "invalid group public key";
    case GS_OUTPUT_BUFFER_TOO_SMALL: return "output buffer too small";
    case GS_INVALID_USER_PRIVATE_KEY: return "invalid user private key";
    case GS_INVALID_JOIN_RESPONSE: return "invalid join response";
    case GS_INVALID_USER_CREDENTIALS: return "invalid user credentials";
    case GS_NOT_SET_GROUP_PRIVATE_KEY: return "group private key not set";
    case GS_NOT_SET_GROUP_PUBLIC_KEY: return "group public key not set";
    case GS_NOT_SET_USER_CREDENTIALS: return "user credentials not set";
    case GS_INVALID_JOIN_MESSAGE: return "invalid join message";
    case GS_INVALID_SIGNATURE: return "invalid signature";
    default: return "unknown message";
  }
}
