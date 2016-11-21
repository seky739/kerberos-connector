#define CC /*
if test -z "${JAVA_HOME}"; then
	for JAVA_HOME in /usr/lib/jvm/java /usr/lib/jvm/default-java; do
		if test -d ${JAVA_HOME}; then
			break
		fi
	done
fi
CPPFLAGS="-I. -I${JAVA_HOME}/include -I${JAVA_HOME}/include/linux -D_GNU_SOURCE"
CFLAGS="-fPIC -W -Wall -g -O0"
LIBS="-lkrb5 -lkadm5clnt_mit"
gcc $CPPFLAGS $CFLAGS -DKRBCONN_TEST java_access.c $0 -o krbconn_test $LIBS || exit $?
gcc $CPPFLAGS $CFLAGS java_access.c $0 -o libkerberos-connector.so -shared -Wl,-soname,libkerberos-connector.so $LIBS || exit $?
exit 0
*/
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <profile.h>
#include "kerberos.h"
#include "cz_zcu_KerberosConnector.h"
#include "java_access.h"


char *krbconn_error(krbconn_context_t *ctx, long code) {
	const char *krbmsg;
	char *text;

	if (ctx->krb) {
		krbmsg = krb5_get_error_message(ctx->krb, code);
		asprintf(&text, "Kerberos error %ld: %s", code, krbmsg);
		krb5_free_error_message(ctx->krb, krbmsg);
	} else {
		asprintf(&text, "Kerberos error %ld: (no details)", code);
	}

	return text;
}


long krbconn_renew(krbconn_context_t *ctx, krbconn_config_t *config) {
	kadm5_config_params params;
	kadm5_ret_t code = 0;
	void *handle = NULL;

	if (ctx->handle) {
		kadm5_destroy(ctx->handle);
		ctx->handle = NULL;
	}

	memset(&params, 0, sizeof params);
	params.mask |= KADM5_CONFIG_REALM;
	params.realm = config->realm;

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

	if (!config->realm) {
		char *realm;

		code = krb5_get_default_realm(krb, &realm);
		if (code != 0) return code;
		config->realm = strdup(realm);
		krb5_free_default_realm(krb, realm);
	}

	return krbconn_renew(ctx, config);
}


void krbconn_destroy(krbconn_context_t *ctx) {
	if (ctx->handle) kadm5_destroy(ctx->handle);
	if (ctx->krb) krb5_free_context(ctx->krb);
	memset(ctx, 0, sizeof(*ctx));
}


void krbconn_free_config(krbconn_config_t *config) {
	free(config->keytab);
	free(config->principal);
	free(config->password);
	free(config->realm);
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
	result->princ_expire = krbresult.princ_expire_time;
	result->pwd_expire = krbresult.pw_expiration;
	result->pwd_change = krbresult.last_pwd_change;
	if ((code = krbconn_princ2str(ctx->krb, krbresult.mod_name, &result->mod_name)) != 0) return code;
	result->mod_date = krbresult.mod_date;
	result->attributes = krbresult.attributes;
	result->policy = strdup(krbresult.policy);

	kadm5_free_principal_ent(ctx->handle, &krbresult);

	return 0;
}


long krbconn_create(krbconn_context_t *ctx, krbconn_principal_t *info, char *pass) {
	kadm5_principal_ent_rec krbprinc;
	krb5_principal krbname;
	long mask = 0;
	long code;

	if ((code = krb5_parse_name(ctx->krb, info->name, &krbname)) != 0) return code;

	memset(&krbprinc, 0, sizeof krbprinc);
	krbprinc.principal = krbname;
	mask |= KADM5_PRINCIPAL;
	if (info->princ_expire) {
		mask |= KADM5_PRINC_EXPIRE_TIME;
		krbprinc.princ_expire_time = info->princ_expire;
	}
	if (info->pwd_expire) {
		mask |= KADM5_PW_EXPIRATION;
		krbprinc.pw_expiration = info->pwd_expire;
	}
	if (info->attributes) {
		mask |= KADM5_ATTRIBUTES;
		krbprinc.attributes = info->attributes;
	}
	if (info->policy) {
		mask |= KADM5_POLICY;
		krbprinc.policy = info->policy;
	}
	code = kadm5_create_principal(ctx->handle, &krbprinc, mask, pass);
	krb5_free_principal(ctx->krb, krbname);
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

	cls = (*env)->GetObjectClass(env, config);

	krbconn_fill_config(env, config, &conf, gs_accessor);

	//Initialize context
	long code;
	if ((code = krbconn_init(ctx, &conf)) != 0) {
		err = krbconn_error(ctx, code);
		printf("%s\n", err);
		free(err);
		//TODO: throw exception
		return;
	}

	//Store context
	cls = (*env)->GetObjectClass(env, this);
	fid = (*env)->GetFieldID(env, cls, "contextPointer", "J");
	(*env)->SetLongField(env, this, fid, (jlong)ctx);

	krbconn_free_config(&conf);
}


JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1destroy(JNIEnv *env, jobject this) {
	krbconn_context_t* ctx = getContext(env, this);

	krbconn_destroy(ctx);
	free(ctx);
}


JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1renew(JNIEnv *env, jobject this) {
	krbconn_context_t* ctx = getContext(env, this);

	//TODO: actually renew the ticket
}

#ifdef KRBCONN_TEST
void usage(const char *name) {
	printf("Usage: %s [OPTIONS] [get|create]\n\
OPTIONS are:\n\
  -h ............. usage\n\
  -k FILE ........ keytab file\n\
  -u PRINCIPAL ... admin principal\n\
  -p PASSWORD .... admin password\n\
", name);
}


int main(int argc, char **argv) {
	krbconn_config_t config;
	krbconn_context_t ctx;
	long code = 0;
	char *err;
	krbconn_principal_t principal;
	char c;
	const char *command = "get";

	memset(&config, 0, sizeof config);
	while ((c = getopt(argc, argv, "hu:p:k:r:")) != -1) {
		switch(c) {
			case 'h':
				usage(argv[0]);
				return 0;
			case 'k':
				config.keytab = strdup(optarg);
				break;
			case 'u':
				config.principal = strdup(optarg);
				break;
			case 'p':
				config.password = strdup(optarg);
				break;
			case 'r':
				config.realm = strdup(optarg);
				break;
		}
	}
	if (optind < argc) {
		command = argv[optind];
	}
	if (!config.principal) {
		usage(argv[0]);
		printf("\n");
		printf("Admin principal name required\n");
		krbconn_free_config(&config);
		return 1;
	}
	if (!config.keytab && !config.password) {
		usage(argv[0]);
		printf("\n");
		printf("Keytab file or password required\n");
		krbconn_free_config(&config);
		return 1;
	}

	if ((code = krbconn_init(&ctx, &config)) != 0) {
		err = krbconn_error(&ctx, code);
		printf("%s\n", err);
		free(err);
		return code;
	}

	if (strcmp(command, "get") == 0) {
		if ((code = krbconn_get(&ctx, "majlen", &principal))) {
			err = krbconn_error(&ctx, code);
			printf("%s\n", err);
			free(err);
			goto end;
		}
		printf("Principal:       %s\n", principal.name);
		printf("Expire:          %s", ctime(&principal.princ_expire));
		printf("Modified:        %s", ctime(&principal.mod_date));
		printf("Modified by:     %s\n", principal.mod_name);
		printf("Password change: %s", ctime(&principal.pwd_change));
		printf("Password expire: %s", ctime(&principal.pwd_expire));
		printf("Attributes:      %d\n", principal.attributes);
		printf("Policy:          %s\n", principal.policy);
		krbconn_free_principal(&principal);
	} else if (strcmp(command, "create") == 0) {
		memset(&principal, 0, sizeof principal);
		principal.name = "host/pokuston.civ.zcu.cz@ZCU.CZ";
		principal.policy = "default_nohistory";
		if ((code = krbconn_create(&ctx, &principal, NULL))) {
			err = krbconn_error(&ctx, code);
			printf("%s\n", err);
			free(err);
			goto end;
		}
		printf("%s created\n", principal.name);
	} else if (strcmp(command, "delete") == 0) {
		principal.name = "host/pokuston.civ.zcu.cz@ZCU.CZ";
		if ((code = krbconn_delete(&ctx, principal.name))) {
			err = krbconn_error(&ctx, code);
			printf("%s\n", err);
			free(err);
			goto end;
		}
		printf("%s deleted\n", principal.name);
	}

end:
	krbconn_destroy(&ctx);
	krbconn_free_config(&config);
	return code;
}
#endif
