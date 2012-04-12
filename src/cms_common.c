/*
 * Copyright 2011-2012 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "pesign.h"

#include <nspr4/prerror.h>

#include <nss3/nss.h>
#include <nss3/secport.h>
#include <nss3/secpkcs7.h>
#include <nss3/secder.h>
#include <nss3/base64.h>
#include <nss3/pk11pub.h>
#include <nss3/secerr.h>

int
cms_context_init(cms_context *ctx)
{
	SECStatus status;
	
	status = NSS_InitReadWrite("/etc/pki/pesign");
	if (status != SECSuccess)
		return -1;

	status = register_oids();
	if (status != SECSuccess)
		return -1;

	memset(ctx, '\0', sizeof (*ctx));

	ctx->arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
	if (!ctx->arena) {
		fprintf(stderr, "Could not create cryptographic arena: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		return -1;
	}

	return 0;
}

void
cms_context_fini(cms_context *ctx)
{
	if (ctx->cert) {
		CERT_DestroyCertificate(ctx->cert);
		ctx->cert = NULL;
	}

	if (ctx->privkey) {
		free(ctx->privkey);
		ctx->privkey = NULL;
	}

	if (ctx->pe_digest) {
		free_poison(ctx->pe_digest->data, ctx->pe_digest->len);
		/* XXX sure seems like we should be freeing it here, but
		 * that's segfaulting, and we know it'll get cleaned up with
		 * PORT_FreeArena a couple of lines down.
		 */
		ctx->pe_digest = NULL;
	}

	if (ctx->ci_digest) {
		free_poison(ctx->ci_digest->data, ctx->ci_digest->len);
		/* XXX sure seems like we should be freeing it here, but
		 * that's segfaulting, and we know it'll get cleaned up with
		 * PORT_FreeArena a couple of lines down.
		 */
		ctx->ci_digest = NULL;
	}


	PORT_FreeArena(ctx->arena, PR_TRUE);
	memset(ctx, '\0', sizeof(*ctx));

	NSS_Shutdown();
}

/* read a cert generated with:
 * $ openssl genrsa -out privkey.pem 2048
 * $ openssl req -new -key privkey.pem -out cert.csr
 * $ openssl req -new -x509 -key privkey.pem -out cacert.pem -days 1095
 * See also: http://www.openssl.org/docs/HOWTO/keys.txt
 */
int read_cert(int certfd, CERTCertificate **cert)
{
	char *certstr = NULL;
	size_t certlen = 0;
	int rc;

	rc = read_file(certfd, &certstr, &certlen);
	if (rc < 0)
		return -1;

	*cert = CERT_DecodeCertFromPackage(certstr, certlen);
	free(certstr);
	if (!*cert)
		return -1;
	return 0;
}

int
generate_octet_string(cms_context *ctx, SECItem *encoded, SECItem *original)
{
	if (SEC_ASN1EncodeItem(ctx->arena, encoded, original,
			SEC_OctetStringTemplate) == NULL)
		return -1;
	return 0;
}

int
generate_object_id(cms_context *ctx, SECItem *encoded, SECOidTag tag)
{
	SECOidData *oid;

	oid = SECOID_FindOIDByTag(tag);
	if (!oid)
		return -1;

	if (SEC_ASN1EncodeItem(ctx->arena, encoded, &oid->oid,
			SEC_ObjectIDTemplate) == NULL)
		return -1;
	return 0;
}

SEC_ASN1Template AlgorithmIDTemplate[] = {
	{
	.kind = SEC_ASN1_SEQUENCE,
	.offset = 0,
	.sub = NULL,
	.size = sizeof (SECAlgorithmID),
	},
	{
	.kind = SEC_ASN1_OBJECT_ID,
	.offset = offsetof(SECAlgorithmID, algorithm),
	.sub = NULL,
	.size = 0,
	},
	{
	.kind = SEC_ASN1_OPTIONAL |
		SEC_ASN1_ANY,
	.offset = offsetof(SECAlgorithmID, parameters),
	.sub = NULL,
	.size = 0,
	},
	{ 0, }
};

int
generate_algorithm_id(cms_context *ctx, SECAlgorithmID *idp, SECOidTag tag)
{
	SECAlgorithmID id;

	if (!idp)
		return -1;

	SECOidData *oiddata;
	oiddata = SECOID_FindOIDByTag(tag);
	if (!oiddata) {
		PORT_SetError(SEC_ERROR_INVALID_ALGORITHM);
		return -1;
	}
	if (SECITEM_CopyItem(ctx->arena, &id.algorithm, &oiddata->oid))
		return -1;

	SECITEM_AllocItem(ctx->arena, &id.parameters, 2);
	if (id.parameters.data == NULL)
		goto err;
	id.parameters.data[0] = SEC_ASN1_NULL;
	id.parameters.data[1] = 0;
	id.parameters.type = siBuffer;

	memcpy(idp, &id, sizeof (id));
	return 0;

err:
	SECITEM_FreeItem(&id.algorithm, PR_FALSE);
	return -1;
}

/* Generate DER for SpcString, which is always "<<<Obsolete>>>" in UCS-2.
 * Irony abounds. Needs to decode like this:
 *        [0]  (28)
 *           00 3c 00 3c 00 3c 00 4f 00 62 00 73 00 6f 00
 *           6c 00 65 00 74 00 65 00 3e 00 3e 00 3e
 */
SEC_ASN1Template SpcStringTemplate[] = {
	{
	.kind = SEC_ASN1_CONTEXT_SPECIFIC | 0,
	.offset = offsetof(SpcString, unicode),
	.sub = &SEC_BMPStringTemplate,
	.size = sizeof (SECItem),
	},
	{ 0, }
};

int
generate_spc_string(PRArenaPool *arena, SECItem *ssp, char *str, int len)
{
	SpcString ss;
	memset(&ss, '\0', sizeof (ss));

	SECITEM_AllocItem(arena, &ss.unicode, len);
	if (!ss.unicode.data)
		return -1;

	memcpy(ss.unicode.data, str, len);
	ss.unicode.type = siBMPString;

	if (SEC_ASN1EncodeItem(arena, ssp, &ss, SpcStringTemplate) == NULL) {
		fprintf(stderr, "Could not encode SpcString: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		return -1;
	}

	return 0;
}

/* Generate the SpcLink DER. Awesomely, this needs to decode as:
 *                      C-[2]  (30)
 * That is all.
 */
SEC_ASN1Template SpcLinkTemplate[] = {
	{
	.kind = SEC_ASN1_CHOICE,
	.offset = offsetof(SpcLink, type),
	.sub = NULL,
	.size = sizeof (SpcLink)
	},
	{
	.kind = SEC_ASN1_CONTEXT_SPECIFIC | 0 |
		SEC_ASN1_EXPLICIT,
	.offset = offsetof(SpcLink, url),
	.sub = &SEC_AnyTemplate,
	.size = SpcLinkTypeUrl,
	},
	{
	.kind = SEC_ASN1_CONSTRUCTED |
		SEC_ASN1_CONTEXT_SPECIFIC | 2,
	.offset = offsetof(SpcLink, file),
	.sub = &SpcStringTemplate,
	.size = SpcLinkTypeFile,
	},
	{ 0, }
};

int
generate_spc_link(PRArenaPool *arena, SpcLink *slp, SpcLinkType link_type,
		void *link_data, size_t link_data_size)
{
	SpcLink sl;
	memset(&sl, '\0', sizeof (sl));

	sl.type = link_type;
	switch (sl.type) {
	case SpcLinkTypeFile:
		if (generate_spc_string(arena, &sl.file, link_data,
				link_data_size) < 0) {
			fprintf(stderr, "got here %s:%d\n",__func__,__LINE__);
			return -1;
		}
		break;
	case SpcLinkTypeUrl:
		sl.url.type = siBuffer;
		sl.url.data = link_data;
		sl.url.len = link_data_size;
		break;
	default:
		fprintf(stderr, "Invalid SpcLinkType\n");
		return -1;
	};

	memcpy(slp, &sl, sizeof (sl));
	return 0;
}
