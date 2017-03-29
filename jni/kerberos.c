#include <stdio.h>
#include <string.h>

#include <kadm5/admin.h>
#include <profile.h>

#include "cz_zcu_KerberosConnector.h"
#include "java_access.h"
#include "kerberos.h"


char *krbconn_error(krbconn_context_t *ctx, long code) {
	const char *krbmsg;
	char *text;

	/*
	 * krb5_get_error_message() won't work without connection (server
	 * handle) - using own error message for kadm5 codes set by krbconn code
	 */
	if (ctx->krb && ctx->handle) {
		krbmsg = krb5_get_error_message(ctx->krb, code);
		asprintf(&text, "Kerberos error %ld: %s", code, krbmsg);
		krb5_free_error_message(ctx->krb, krbmsg);
	} else {
		switch(code) {
			case KADM5_BAD_CLIENT_PARAMS:
				asprintf(&text, "Kerberos error %ld: missing credentials", code);
				break;
			default:
				asprintf(&text, "Kerberos error %ld: (no details)", code);
				break;
		}
	}

	return text;
}


long krbconn_renew(krbconn_context_t *ctx, krbconn_config_t *config) {
	kadm5_config_params params;
	kadm5_ret_t code = KADM5_BAD_CLIENT_PARAMS;
	void *handle = NULL;

	if (ctx->handle) {
		kadm5_destroy(ctx->handle);
		ctx->handle = NULL;
	}
	free(ctx->realm);

	if (config->realm) {
		ctx->realm = strdup(config->realm);
	} else {
		char *realm;

		code = krb5_get_default_realm(ctx->krb, &realm);
		if (code != 0) return code;
		ctx->realm = strdup(realm);
		krb5_free_default_realm(ctx->krb, realm);
	}

	memset(&params, 0, sizeof params);
	params.mask |= KADM5_CONFIG_REALM;
	params.realm = ctx->realm;

	if (config->keytab) {
		code = kadm5_init_with_skey(ctx->krb, config->principal, config->keytab, NULL, &params, KADM5_STRUCT_VERSION, KADM5_API_VERSION_4, NULL, &handle);
	} else if (config->password) {
		code = kadm5_init_with_password(ctx->krb, config->principal, config->password, NULL, &params, KADM5_STRUCT_VERSION, KADM5_API_VERSION_4, NULL, &handle);
	}

	if (code != 0) return code;
	ctx->handle = handle;

	return 0;
}


long krbconn_init(krbconn_context_t *ctx, krbconn_config_t *config) {
	krb5_context krb = NULL;
	krb5_error_code code;

	memset(ctx, 0, sizeof(*ctx));
	code = kadm5_init_krb5_context(&krb);
	if (code != 0) return code;
	ctx->krb = krb;

	return krbconn_renew(ctx, config);
}


void krbconn_destroy(krbconn_context_t *ctx) {
	if (!ctx) return;
	if (ctx->handle) kadm5_destroy(ctx->handle);
	if (ctx->krb) krb5_free_context(ctx->krb);
	free(ctx->realm);
	memset(ctx, 0, sizeof(*ctx));
}


void krbconn_free_config(krbconn_config_t *config) {
	free(config->keytab);
	free(config->principal);
	free(config->password);
	free(config->realm);
	memset(config, 0, sizeof(*config));
}


void krbconn_free_principal(krbconn_principal_t *principal) {
	free(principal->name);
	free(principal->mod_name);
	free(principal->policy);
	memset(principal, 0, sizeof(*principal));
}


static krb5_error_code krbconn_princ2str(krb5_context krb, krb5_principal principal, char **name) {
	char *s;
	krb5_error_code code;

	if ((code = krb5_unparse_name(krb, principal, &s)) != 0) return code;
	*name = strdup(s);
	krb5_free_unparsed_name(krb, s);

	return 0;
}


static void krbconn_princ_striprealm(char *name, const char *realm) {
	size_t len, len_realm;

	if (realm) {
		len_realm = strlen(realm);
		len = strlen(name);
		if (len <= len_realm) return;
		if (name[len - len_realm - 1] != '@') return;
		name[len - len_realm - 1] = '\0';
	}
}


/*
 * fill KADM5 principal record according to the specified masks
 */
static long krbconn_fill_princrec(krb5_context krb, kadm5_principal_ent_rec *krbrec, long *mask_out, krbconn_principal_t *info, int mask_in) {
	long code;
	krb5_principal krbprinc;
	long mask = *mask_out;

	if ((code = krb5_parse_name(krb, info->name, &krbprinc)) != 0) return code;

	memset(krbrec, 0, sizeof(*krbrec));
	krbrec->principal = krbprinc;
	if ((mask_in & KRBCONN_PRINC_EXPIRE_TIME) != 0) {
		mask |= KADM5_PRINC_EXPIRE_TIME;
		krbrec->princ_expire_time = info->princ_expire;
	}
	if ((mask_in & KRBCONN_PW_EXPIRATION) != 0) {
		mask |= KADM5_PW_EXPIRATION;
		krbrec->pw_expiration = info->pwd_expire;
	}
	if ((mask_in & KRBCONN_ATTRIBUTES) != 0) {
		mask |= KADM5_ATTRIBUTES;
		krbrec->attributes = info->attributes;
	}
	if ((mask_in & KRBCONN_POLICY) != 0) {
		if (info->policy) {
			mask |= KADM5_POLICY;
			krbrec->policy = info->policy;
		} else {
			mask |= KADM5_POLICY_CLR;
		}
	}

	*mask_out = mask;
	return 0;
}


static void krbconn_free_princrec(krb5_context krb, kadm5_principal_ent_rec *krbrec) {
	krb5_free_principal(krb, krbrec->principal);
	memset(krbrec, 0, sizeof(*krbrec));
}


long krbconn_get(krbconn_context_t *ctx, char *princ_name, krbconn_principal_t *result) {
	long code;
	krb5_principal principal;
	kadm5_principal_ent_rec krbresult;

	code = krb5_parse_name(ctx->krb, princ_name, &principal);
	if (code) return code;

	code = kadm5_get_principal(ctx->handle, principal, &krbresult, KADM5_PRINCIPAL_NORMAL_MASK/* | KADM5_KEY_DATA*/);
	krb5_free_principal(ctx->krb, principal);
	if (code) return code;

	memset(result, 0, sizeof(*result));
	if ((code = krbconn_princ2str(ctx->krb, krbresult.principal, &result->name)) != 0) return code;
	krbconn_princ_striprealm(result->name, ctx->realm);
	result->princ_expire = krbresult.princ_expire_time;
	result->pwd_expire = krbresult.pw_expiration;
	result->pwd_change = krbresult.last_pwd_change;
	if ((code = krbconn_princ2str(ctx->krb, krbresult.mod_name, &result->mod_name)) != 0) return code;
	krbconn_princ_striprealm(result->mod_name, ctx->realm);
	result->mod_date = krbresult.mod_date;
	result->attributes = krbresult.attributes;
	if (krbresult.policy) result->policy = strdup(krbresult.policy);

	kadm5_free_principal_ent(ctx->handle, &krbresult);

	return 0;
}


long krbconn_create(krbconn_context_t *ctx, krbconn_principal_t *info, int mask, char *pass) {
	kadm5_principal_ent_rec krbrec;
	long krbmask = KADM5_PRINCIPAL; /* required (only) during creating */
	long code;

	if ((code = krbconn_fill_princrec(ctx->krb, &krbrec, &krbmask, info, mask)) != 0) return code;
	code = kadm5_create_principal(ctx->handle, &krbrec, krbmask, pass);
	krbconn_free_princrec(ctx->krb, &krbrec);
	return code;
}


long krbconn_delete(krbconn_context_t *ctx, char *name) {
	krb5_principal krbname;
	long code = 0;

	if ((code = krb5_parse_name(ctx->krb, name, &krbname)) != 0) return code;
	code = kadm5_delete_principal(ctx->handle, krbname);
	krb5_free_principal(ctx->krb, krbname);
	return code;
}


long krbconn_list(krbconn_context_t *ctx, const char *search, char ***list, int *count) {
	long code;
	char *exp = NULL;

	*list = NULL;
	*count = 0;

	/* add realm to the query */
	if (search) {
		if (strchr(search, '@')) {
			exp = strdup(search);
		} else {
			size_t len = strlen(search);
			exp = malloc(len + 1 + strlen(ctx->realm) + 1);
			memcpy(exp, search, len);
			exp[len] = '@';
			strcpy(exp + len + 1, ctx->realm);
		}
	}
	code = kadm5_get_principals(ctx->handle, exp, list, count);
	free(exp);

	return code;
}


void krbconn_free_list(krbconn_context_t *ctx, char **list, int count) {
	kadm5_free_name_list(ctx->handle, list, count);
}


long krbconn_modify(krbconn_context_t *ctx, krbconn_principal_t *info, int mask) {
	kadm5_principal_ent_rec krbrec;
	long krbmask = 0;
	long code;

	if ((code = krbconn_fill_princrec(ctx->krb, &krbrec, &krbmask, info, mask)) != 0) return code;
	code = kadm5_modify_principal(ctx->handle, &krbrec, krbmask);
	krbconn_free_princrec(ctx->krb, &krbrec);
	return code;
}


long krbconn_rename(krbconn_context_t *ctx, const char *oldname, const char *newname) {
	krb5_principal oldprinc, newprinc;
	long code = 0;

	if ((code = krb5_parse_name(ctx->krb, oldname, &oldprinc)) != 0) return code;
	if ((code = krb5_parse_name(ctx->krb, newname, &newprinc)) != 0) {
		krb5_free_principal(ctx->krb, oldprinc);
		return code;
	}

	code = kadm5_rename_principal(ctx->handle, oldprinc, newprinc);

	krb5_free_principal(ctx->krb, oldprinc);
	krb5_free_principal(ctx->krb, newprinc);

	return code;
}


long krbconn_chpass(krbconn_context_t *ctx, const char *princ_name, char *password) {
	long code;
	krb5_principal krbprinc;

	if ((code = krb5_parse_name(ctx->krb, princ_name, &krbprinc)) != 0) return code;
	code = kadm5_chpass_principal(ctx->handle, krbprinc, password);
	krb5_free_principal(ctx->krb, krbprinc);

	return code;
}


void krbconn_fill_config(JNIEnv *env, jobject config, krbconn_config_t* conf, jclass gs_accessor) {
	conf->realm = jstring_getter(env, config, "getRealm");
	conf->principal = jstring_getter(env, config, "getPrincipal");
	conf->password = jguardedstring_getter(env, config, "getPassword", gs_accessor);
	conf->keytab = jstring_getter(env, config, "getKeytab");
}


JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1init(JNIEnv * env , jobject this, jclass gs_accessor) {
	krbconn_context_t* ctx = calloc(sizeof(krbconn_context_t), 1);
	krbconn_config_t conf;
	char* err;

	jfieldID fid;
	jclass cls;

	//Get configuration from KerberosConfiguration
	cls = (*env)->GetObjectClass(env, this);
	fid = (*env)->GetFieldID(env, cls, "configuration", "Lcz/zcu/KerberosConfiguration;");
	jobject config = (*env)->GetObjectField(env, this, fid);

	krbconn_fill_config(env, config, &conf, gs_accessor);
	(*env)->DeleteLocalRef(env, config);

	//Initialize context
	long code;
	if ((code = krbconn_init(ctx, &conf)) != 0) {
		err = krbconn_error(ctx, code);
		throwKerberosException(env, err);
		free(err);
		return;
	}

	//Store context
	fid = (*env)->GetFieldID(env, cls, "contextPointer", "J");
	(*env)->SetLongField(env, this, fid, (jlong)ctx);
	(*env)->DeleteLocalRef(env, cls);

	krbconn_free_config(&conf);
}


JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1destroy(JNIEnv *env, jobject this) {
	krbconn_context_t* ctx = getContext(env, this);
	jclass cls;
	jfieldID fid;

	krbconn_destroy(ctx);
	free(ctx);

	//Clear context
	cls = (*env)->GetObjectClass(env, this);
	fid = (*env)->GetFieldID(env, cls, "contextPointer", "J");
	(*env)->SetLongField(env, this, fid, (jlong)NULL);
	(*env)->DeleteLocalRef(env, cls);
}


JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1renew(JNIEnv *env, jobject this, jclass gs_accessor) {
	krbconn_context_t* ctx = getContext(env, this);
	krbconn_config_t conf;
	jfieldID fid;
	jclass cls;

	//Get configuration from KerberosConfiguration
	cls = (*env)->GetObjectClass(env, this);
	fid = (*env)->GetFieldID(env, cls, "configuration", "Lcz/zcu/KerberosConfiguration;");

	(*env)->DeleteLocalRef(env, cls);
	jobject config = (*env)->GetObjectField(env, this, fid);

	krbconn_fill_config(env, config, &conf, gs_accessor);
	(*env)->DeleteLocalRef(env, config);

	long code;
	if ((code = krbconn_renew(ctx, &conf)) != 0) {
		char* err = krbconn_error(ctx, code);
		throwKerberosException(env, err);
		free(err);
		return;
	}

	krbconn_free_config(&conf);
}

JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1create(JNIEnv *env, jobject this, jstring name, jstring pass,
                                                                  jlong princ_expiry, jlong pass_expiry,
                                                                  jint attrs, jstring policy, jint mask) {
	krbconn_context_t* ctx = getContext(env, this);
	krbconn_principal_t* princ = calloc(sizeof(krbconn_principal_t), 1);

	const char* temp;
	char* str;
	char *pass_str = NULL;

	if (name != NULL) {
		temp = (*env)->GetStringUTFChars(env, name, 0);
		str = strdup(temp);
		(*env)->ReleaseStringUTFChars(env, name, temp);
		(*env)->DeleteLocalRef(env, name);
		princ->name = str;
	}

	princ->princ_expire = princ_expiry;
	princ->pwd_expire = pass_expiry;
	princ->attributes = attrs;

	if (policy != NULL) {
		temp = (*env)->GetStringUTFChars(env, policy, 0);
		str = strdup(temp);
		(*env)->ReleaseStringUTFChars(env, policy, temp);
		(*env)->DeleteLocalRef(env, policy);
		princ->policy = str;
	}

	if (pass != NULL) {
		temp = (*env)->GetStringUTFChars(env, pass, 0);
		pass_str = strdup(temp);
		(*env)->ReleaseStringUTFChars(env, pass, temp);
		(*env)->DeleteLocalRef(env, pass);
	}

	long err = krbconn_create(ctx, princ, mask, pass_str);
	free(pass_str);
	krbconn_free_principal(princ);
	free(princ);

	if (err != 0) {
		char* errMsg = krbconn_error(ctx, err);
        throwKerberosException(env, errMsg);
		free(errMsg);
        return;
	}
}

JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1delete(JNIEnv *env, jobject this, jstring name) {
	krbconn_context_t* ctx = getContext(env, this);
	const char* temp;
	char* str = NULL;

	if (name != NULL) {
		temp = (*env)->GetStringUTFChars(env, name, 0);
		str = strdup(temp);
		(*env)->ReleaseStringUTFChars(env, name, temp);
		(*env)->DeleteLocalRef(env, name);
	}

	long err = krbconn_delete(ctx, str);
	free(str);

	if (err != 0) {
		char* errMsg = krbconn_error(ctx, err);
		throwKerberosException(env, errMsg);
		free(errMsg);
	}
}

JNIEXPORT jobject JNICALL Java_cz_zcu_KerberosConnector_krb5_1search(JNIEnv *env, jobject this, jstring query,
 	                                                                      jint pageSize, jint pageOffset) {
	krbconn_context_t* ctx = getContext(env, this);
	char* cQuery = NULL;
	if (query != NULL) {
		const char* temp = (*env)->GetStringUTFChars(env, query, 0);
		cQuery = strdup(temp);
		(*env)->ReleaseStringUTFChars(env, query, temp);
		(*env)->DeleteLocalRef(env, query);
	}

	char** list;
	int count;
	krbconn_list(ctx, cQuery, &list, &count);
	free(cQuery);

	int trueCount = count;
	if (count - pageOffset < pageSize) {
		trueCount = count - pageOffset;
	} else if (pageSize != 0) {
		trueCount = pageSize;
	}

	if (count < pageOffset) {
		printf("Count of found principals %d is less then requested offset %d\n", count, pageOffset);
		return NULL;
	}

	static jclass arrClass = NULL;
	if (arrClass == NULL) {
		arrClass = (*env)->FindClass(env, "cz/zcu/KerberosPrincipal");
		arrClass = (*env)->NewGlobalRef(env, arrClass);
	}

	jobjectArray arr = (*env)->NewObjectArray(env, trueCount, arrClass, NULL);

	for (int i = pageOffset; i < pageOffset + trueCount; i++) {
		krbconn_principal_t princ;
		memset(&princ, 0, sizeof(princ));

		krbconn_get(ctx, list[i], &princ);
		add_princ_to_array(env, arr, i - pageOffset, princ, arrClass);
		krbconn_free_principal(&princ);
	}

	krbconn_free_list(ctx, list, count);

	static jclass results = NULL;
	static jmethodID mid = NULL;
	if (results == NULL) {
		results = (*env)->FindClass(env, "cz/zcu/KerberosSearchResults");
		results = (*env)->NewGlobalRef(env, results);
	}
	if (mid == NULL) {
		mid = (*env)->GetMethodID(env, results, "<init>", "([Lcz/zcu/KerberosPrincipal;I)V");
	}

	jint remaining = count - pageOffset - trueCount;
	jobject out = (*env)->NewObject(env, results, mid, arr, remaining);

	(*env)->DeleteLocalRef(env, arr);

	return out;
}

JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1rename(JNIEnv *env, jobject this, jstring name, jstring newName) {
	krbconn_context_t* ctx = getContext(env, this);
	const char* temp;

	temp = (*env)->GetStringUTFChars(env, name, 0);
	char* princ_old_name = strdup(temp);
	(*env)->ReleaseStringUTFChars(env, name, temp);
	(*env)->DeleteLocalRef(env, name);

	temp = (*env)->GetStringUTFChars(env, newName, 0);
	char* princ_new_name = strdup(temp);
	(*env)->ReleaseStringUTFChars(env, newName, temp);
	(*env)->DeleteLocalRef(env, newName);

	long err = krbconn_rename(ctx, princ_old_name, princ_new_name);
	free(princ_new_name);
	free(princ_old_name);

	if (err != 0) {
		char* errMsg = krbconn_error(ctx, err);
		throwKerberosException(env, errMsg);
		free(errMsg);
	}
}

JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1chpasswd(JNIEnv *env, jobject this, jstring name, jstring password) {
	krbconn_context_t* ctx = getContext(env, this);
	const char* temp;

	temp = (*env)->GetStringUTFChars(env, name, 0);
	char* princ_name = strdup(temp);
	(*env)->ReleaseStringUTFChars(env, name, temp);
	(*env)->DeleteLocalRef(env, name);

	temp = (*env)->GetStringUTFChars(env, password, 0);
	char* princ_pass = strdup(temp);
	(*env)->ReleaseStringUTFChars(env, password, temp);
	(*env)->DeleteLocalRef(env, password);

	long err = krbconn_chpass(ctx, princ_name, princ_pass);
	free(princ_name);
	free(princ_pass);

	if (err != 0) {
		char* errMsg = krbconn_error(ctx, err);
		throwKerberosException(env, errMsg);
		free(errMsg);
	}
}

JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1modify(JNIEnv *env, jobject this, jstring name,
                                                                     jlong princ_expiry, jlong password_expiry,
                                                                     jint attributes, jstring policy, jint mask) {
	krbconn_context_t* ctx = getContext(env, this);
	const char* temp;

	temp = (*env)->GetStringUTFChars(env, name, 0);
	char* princ_name = strdup(temp);
	(*env)->ReleaseStringUTFChars(env, name, temp);
	(*env)->DeleteLocalRef(env, name);

	krbconn_principal_t* princ = calloc(sizeof(krbconn_principal_t), 1);
	krbconn_get(ctx, princ_name, princ);

	if ((mask & KRBCONN_PRINC_EXPIRE_TIME) != 0) {
		princ->princ_expire = princ_expiry;
	}

	if ((mask & KRBCONN_PW_EXPIRATION) != 0) {
		princ->pwd_expire = password_expiry;
	}

	if ((mask & KRBCONN_ATTRIBUTES) != 0) {
		princ->attributes = attributes;
	}

	if ((mask & KRBCONN_POLICY) != 0) {
		temp = (*env)->GetStringUTFChars(env, policy, 0);
		princ->policy = strdup(temp);
		(*env)->ReleaseStringUTFChars(env, policy, temp);
		(*env)->DeleteLocalRef(env, policy);
	}

	long err = krbconn_modify(ctx, princ, mask);
	krbconn_free_principal(princ);
	free(princ);
	free(princ_name);

	if (err != 0) {
		char* errMsg = krbconn_error(ctx, err);
		throwKerberosException(env, errMsg);
		free(errMsg);
	}
}
