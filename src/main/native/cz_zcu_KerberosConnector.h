/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class cz_zcu_KerberosConnector */

#ifndef _Included_cz_zcu_KerberosConnector
#define _Included_cz_zcu_KerberosConnector
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     cz_zcu_KerberosConnector
 * Method:    krb5_init
 * Signature: (Ljava/lang/Class;)V
 */
JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1init
  (JNIEnv *, jobject, jclass);

/*
 * Class:     cz_zcu_KerberosConnector
 * Method:    krb5_destroy
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1destroy
  (JNIEnv *, jobject);

/*
 * Class:     cz_zcu_KerberosConnector
 * Method:    krb5_renew
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1renew
  (JNIEnv *, jobject);

/*
 * Class:     cz_zcu_KerberosConnector
 * Method:    krb5_create
 * Signature: (Ljava/lang/String;Ljava/lang/String;JJILjava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1create
  (JNIEnv *, jobject, jstring, jstring, jlong, jlong, jint, jstring);

/*
 * Class:     cz_zcu_KerberosConnector
 * Method:    krb5_delete
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_cz_zcu_KerberosConnector_krb5_1delete
  (JNIEnv *, jobject, jstring);

/*
 * Class:     cz_zcu_KerberosConnector
 * Method:    krb5_search
 * Signature: (Ljava/lang/String;)[Lcz/zcu/KerberosPrincipal;
 */
JNIEXPORT jobjectArray JNICALL Java_cz_zcu_KerberosConnector_krb5_1search
  (JNIEnv *, jobject, jstring);

#ifdef __cplusplus
}
#endif
#endif
