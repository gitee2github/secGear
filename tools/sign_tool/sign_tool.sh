#!/bin/bash
API_LEVEL=2
ONE_STEP_MODE=1

signtoolpath="/opt/itrustee_sdk/build/signtools/"

print_help() {
    echo "sign tool usage: ./sign_tool.sh [options] ..."
    echo "[options]"
    echo "-c <file>       basic config file."
    echo "-d <parameter>  sign tool command, sign/digest/dump."
    echo "                The sign command is used to generate a signed enclave."
    echo "                The digest command is used to generate signing material."
    echo "                The dump command is used to generate metadata for sgx signed enclave."
    echo "-i <file>       input parameter, which is enclave to be signed for digest/sign command, and signed enclave for"
    echo "                dump command."
    echo "-k <file>       private key required for single-step method. Note: single-step method is only for the debug mode,"
    echo "-k <file>       plaintext private key does exist in the production environment."
    echo "-m <file>       additional config_cloud.ini for trustzone."
    echo "-o <file>       output parameter. "
    echo "                sgx:  the sign command outputs signed enclave, the digest command outputs signing"
    echo "                      material, the dump command outputs data containing the SIGStruct metadata for the SGX signed"
    echo "                      enclave, which is submitted to Intel for whitelisting."
    echo "                trustzone: /output_path/uuid.sec, uuid must be the same as the value of gpd.ta.appID in manifest.txt"
    echo "-p <file>       signing server public key certificate, required for sgx two-step method."
    echo "-s <file>       the signature value required for two-step method, this parameter is empty to indicate"
    echo "                single-step method."
    echo "-x <parameter>  enclave type, sgx or trustzone."
    echo "-h              print help message."

}

while getopts "c:d:i:k:m:o:p:s:x:h" opt; do
    case $opt in
        c)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -c is missing or incorrect"
                exit 1
            fi
            CONFIG_FILE=$OPTARG
            ;;
        d)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -d is missing or incorrect"
                exit 1
            fi
            typeset -l CMD
            CMD=$OPTARG
            ;;
        i)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -i is missing or incorrect"
                exit 1
            fi
            IN_ENCLAVE=$OPTARG
            ;;
        k)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -k is missing or incorrect"
                exit 1
            fi
            SIG_KEY=$OPTARG
            ;;
        m)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -m is missing or incorrect"
                exit 1
            fi
            A_CONFIG_FILE=$OPTARG
            ;;
        o)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -o is missing or incorrect"
                exit 1
            fi
            OUT_FILE=$OPTARG
            ;;
        p)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -p is missing or incorrect"
                exit 1
            fi
            SERVER_PUBKEY=$OPTARG
            ;;
        s)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -s is missing or incorrect"
                exit 1
            fi
            SIGNATURE=$OPTARG
            ;;
        x)
            if [[ $OPTARG == -* ]]; then
                echo "Error: parameter for -x is missing or incorrect"
                exit 1
            fi
            typeset -l ENCLAVE_TYPE
            ENCLAVE_TYPE=$OPTARG
            ;;
        h)
            print_help
            exit 0
            ;;
        ?)
            print_help
            exit 1
            ;;
    esac
done
if [ ${OPTIND} == 1 ]; then
    print_help
    exit 0
fi

itrustee_start_sign() {
    #    check_native_sign
    if [ -z "$A_CONFIG_FILE" ]; then
        echo "Error: missing additional config_cloud.ini file for signing iTrustee enclave"
        exit 1
    fi
    if [ -z $CONFIG_FILE ]; then
        echo "Error: missing basic config file for signing iTrustee enclave"
        exit 1
    fi
    if [ ! -e $CONFIG_FILE ]; then
        echo "No such file or directory"
        exit 1
    fi
    if [ -z $IN_ENCLAVE ]; then
        echo "Error: missing enclave file"
        exit 1
    fi
    if [ ! -e $IN_ENCLAVE ]; then
        echo "Error: No such file or directory"
        exit 1
    fi

    if [ "${CMD}"x == "sign"x ]; then
        IN_PATH=$(dirname ${CONFIG_FILE})
        cp ${IN_ENCLAVE} ${IN_PATH}/libcombine.so
        OUT_PATH=$(dirname ${OUT_FILE})
        echo ${IN_PATH} ${OUT_PATH}
        echo "CFLAGS += -DAPI_LEVEL=${API_LEVEL}" > ${IN_PATH}/config.mk
        python3 -B ${signtoolpath}/signtool_v3.py ${IN_PATH} ${OUT_PATH} --privateCfg ${A_CONFIG_FILE}
        rm -f ${IN_PATH}/config.mk
        rm -f ${IN_PATH}/libcombine.so
    else
        echo "Error: illegal command"
    fi
}

sgx_start_sign() {
    if [ -z "$IN_ENCLAVE" ]; then
        echo "Error: missing enclave file"
        exit 1
    fi
    SIGDATA_FILE="signdata"
    if [ "${CMD}"x == "sign"x ]; then
        if [ -z "$SIGNATURE" ]; then
            if [ -z "$SIG_KEY" ]; then
                echo "Error: missing sign key"
                exit 1
            fi
            if [ -z "$CONFIG_FILE" ]; then
                sgx_sign sign -enclave "${IN_ENCLAVE}" -key "${SIG_KEY}" -out "${OUT_FILE}"
            else
                sgx_sign sign -enclave "${IN_ENCLAVE}" -key "${SIG_KEY}" -out "${OUT_FILE}" -config "${CONFIG_FILE}"
            fi
        else
            if [ -z "$SERVER_PUBKEY" ]; then
                echo "Error: missing server public key"
                exit 1
            fi
            if [ -z "$CONFIG_FILE" ]; then
                sgx_sign catsig -enclave "${IN_ENCLAVE}" -key "${SERVER_PUBKEY}" -sig "${SIGNATURE}" -unsigned "${SIGDATA_FILE}" -out "${OUT_FILE}"
            else
                sgx_sign catsig -enclave "${IN_ENCLAVE}" -key "${SERVER_PUBKEY}" -sig "${SIGNATURE}" -unsigned "${SIGDATA_FILE}" -out "${OUT_FILE}" -config "${CONFIG_FILE}"
            fi
            rm -rf ${SIGDATA_FILE}
        fi
    elif [ "${CMD}"x == "digest"x ]; then
        if [ -z "$CONFIG_FILE" ]; then
            sgx_sign gendata -enclave "${IN_ENCLAVE}" -out "${SIGDATA_FILE}"
        else
            sgx_sign gendata -enclave "${IN_ENCLAVE}" -out "${SIGDATA_FILE}" -config "${CONFIG_FILE}"
        fi
        cp "${SIGDATA_FILE}" "${OUT_FILE}"
    elif [ "${CMD}"x == "dump"x ]; then
        sgx_sign dump -enclave "${IN_ENCLAVE}" -dumpfile "${OUT_FILE}"
    else
        echo "Error: illegal command"
    fi
}

if [ -z "$CMD" ]; then
    echo "Error: missing command"
    exit 1
fi
if [ -z "$ENCLAVE_TYPE" ]; then
    echo "Error: missing enclave type"
    exit 1
fi
if [ -z "$OUT_FILE" ]; then
    echo "Error: missing out file"
    exit 1
fi
umask 0077
check_results=$(uname -m)
if [ "${ENCLAVE_TYPE}"x == "sgx"x ]; then
    if [ "${check_results}"x != "x86_64"x ]; then
        echo "Warning: the enclave type does not comply with current architecture"
    fi
    sgx_start_sign
elif [ "${ENCLAVE_TYPE}"x == "trustzone"x ]; then
    if [ "${check_results}"x != "aarch64"x ]; then
        echo "Warning: the enclave type does not comply with current architecture"
    fi
    itrustee_start_sign
else
    echo "Error: illegal enclave type"
    exit 1
fi
