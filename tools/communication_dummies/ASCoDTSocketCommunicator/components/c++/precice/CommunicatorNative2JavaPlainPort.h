#ifndef PRECICE_COMMUNICATOR2NATIVE2JAVAPLAINPORT_H_
#define PRECICE_COMMUNICATOR2NATIVE2JAVAPLAINPORT_H_ 

#include "precice/Communicator.h"
#include <jni.h> 
#include <iostream>
//
// ASCoDT - Advanced Scientific Computing Development Toolkit
//
// This file was generated by ASCoDT's simplified SIDL compiler.
//
// Authors: Tobias Weinzierl, Atanas Atanasov   
//
#ifdef __cplusplus
  extern "C" {
#endif


          
JNIEXPORT void JNICALL Java_precice_CommunicatorNative2JavaPlainPort_createInstance(JNIEnv *env, jobject obj);
JNIEXPORT void JNICALL Java_precice_CommunicatorNative2JavaPlainPort_destroyInstance(JNIEnv *env, jobject obj,jlong ref);

#ifdef __cplusplus
  }
#endif




namespace precice { 

     class CommunicatorNative2JavaPlainPort;
}

class precice::CommunicatorNative2JavaPlainPort: public precice::Communicator{
  private:
    JavaVM* _jvm;
    jobject _obj;
  public:
    CommunicatorNative2JavaPlainPort(JavaVM* jvm,jobject obj);
    ~CommunicatorNative2JavaPlainPort();
    void setData(const double* data, const int data_len);  
    void setDataParallel(const double* data, const int data_len);
   
};
#endif
