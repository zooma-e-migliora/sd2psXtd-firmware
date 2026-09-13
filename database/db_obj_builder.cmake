# Genera il database dei giochi: input -> .dat -> oggetto ELF da linkare.
#
# ATTENZIONE, questo script ha gia' distrutto il database due volte. Il guasto
# non era "non c'e' rete", era l'ORDINE delle operazioni:
#
#   1. il nome dell'oggetto contiene la DATA, quindi il giorno dopo non esiste
#      piu' e il blocco rigenera tutto;
#   2. la prima cosa che faceva era CANCELLARE i gamedb<sys>_* esistenti,
#      cioe' l'unica copia buona;
#   3. poi file(DOWNLOAD) senza controllo di stato: senza rete lascia un file
#      VUOTO invece di fallire;
#   4. il parser girava con OUTPUT_QUIET su un input vuoto e produceva un .dat
#      da 0 byte, objcopy ne faceva un oggetto valido, e il firmware usciva con
#      il database vuoto SENZA UN ERRORE DA NESSUNA PARTE.
#
# Le tre regole che lo impediscono, e vanno lette come una cosa sola:
#   - gli input si prendono dalla CACHE locale se c'e', e si scaricano solo se
#     manca (CACHE_DIR, sotto sd2psXtd/, versionata col progetto);
#   - ogni passo che puo' fallire VIENE CONTROLLATO, e fallisce rumorosamente;
#   - non si cancella niente finche' il sostituto non e' validato.

string(TIMESTAMP date "%Y%m%d")

set(IN_DIR  "${OUTPUT_DIR}/gamedb${SYSTEM}_input")
set(DAT     "gamedb${SYSTEM}.dat")
set(DAT_ABS "${OUTPUT_DIR}/${DAT}")
set(OBJ     "${OUTPUT_DIR}/gamedb${SYSTEM}_${date}.o")

if(NOT EXISTS "${OBJ}")

    find_package(Python3 COMPONENTS Interpreter Development)

    file(MAKE_DIRECTORY "${IN_DIR}")

    # --- 1. gli input: prima la cache, la rete solo se manca ----------------
    foreach(url ${INPUT_URLS})
        get_filename_component(name "${url}" NAME)
        set(cached "${CACHE_DIR}/${name}")
        set(dest   "${IN_DIR}/${name}")

        if(EXISTS "${cached}")
            file(SIZE "${cached}" cached_size)
            if(cached_size EQUAL 0)
                message(FATAL_ERROR
                    "database ${SYSTEM}: la copia in cache e' VUOTA:\n"
                    "  ${cached}\n"
                    "Cancellala e riscaricala da:\n  ${url}")
            endif()
            message(STATUS "database ${SYSTEM}: uso la cache ${name} (${cached_size} byte)")
            file(COPY_FILE "${cached}" "${dest}")
        else()
            message(STATUS "database ${SYSTEM}: ${name} non e' in cache, lo scarico")
            file(DOWNLOAD "${url}" "${dest}" STATUS st TIMEOUT 120)
            list(GET st 0 code)
            if(NOT code EQUAL 0)
                list(GET st 1 msg)
                file(REMOVE "${dest}")
                message(FATAL_ERROR
                    "database ${SYSTEM}: scaricamento FALLITO (${code}: ${msg})\n"
                    "  ${url}\n"
                    "Senza rete, scarica il file a mano e mettilo in:\n"
                    "  ${CACHE_DIR}/${name}\n"
                    "Prima questo caso produceva un database VUOTO in silenzio.")
            endif()
            file(SIZE "${dest}" got)
            if(got EQUAL 0)
                file(REMOVE "${dest}")
                message(FATAL_ERROR
                    "database ${SYSTEM}: scaricato un file VUOTO da\n  ${url}")
            endif()
        endif()
    endforeach()

    # --- 2. il .dat, in una posizione di lavoro -----------------------------
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${REPO_ROOT}/ext/unidecode"
                ${Python3_EXECUTABLE} ${PYTHON_SCRIPT}
                ${SYSTEM} ${OUTPUT_DIR} "${IN_DIR}" "${DAT}.new"
        WORKING_DIRECTORY ${OUTPUT_DIR}
        RESULT_VARIABLE parse_rc
        OUTPUT_VARIABLE parse_out
        ERROR_VARIABLE  parse_err)
    if(NOT parse_rc EQUAL 0)
        message(FATAL_ERROR
            "database ${SYSTEM}: il parser e' uscito con ${parse_rc}\n"
            "${parse_out}\n${parse_err}")
    endif()

    # --- 3. la validazione, che e' il punto di tutto ------------------------
    if(NOT EXISTS "${DAT_ABS}.new")
        message(FATAL_ERROR "database ${SYSTEM}: il parser non ha prodotto ${DAT}.new")
    endif()
    file(SIZE "${DAT_ABS}.new" dat_size)
    if(dat_size LESS 1024)
        file(REMOVE "${DAT_ABS}.new")
        message(FATAL_ERROR
            "database ${SYSTEM}: il .dat generato e' di ${dat_size} byte, "
            "troppo pochi per essere vero.\n"
            "La copia precedente NON e' stata toccata.\n"
            "${parse_out}")
    endif()
    message(STATUS "database ${SYSTEM}: generati ${dat_size} byte")

    # --- 4. solo ORA si sostituisce il vecchio ------------------------------
    file(GLOB stale "${OUTPUT_DIR}/gamedb${SYSTEM}_*.o")
    foreach(f ${stale})
        file(REMOVE "${f}")
    endforeach()
    file(RENAME "${DAT_ABS}.new" "${DAT_ABS}")

    execute_process(
        COMMAND ${CMAKE_OBJCOPY} --input-target=binary --output-target=elf32-littlearm
                --binary-architecture arm --rename-section .data=.rodata
                "${DAT}" "${OBJ}"
        WORKING_DIRECTORY ${OUTPUT_DIR}
        RESULT_VARIABLE oc_rc)
    if(NOT oc_rc EQUAL 0)
        message(FATAL_ERROR "database ${SYSTEM}: objcopy e' uscito con ${oc_rc}")
    endif()

    file(REMOVE_RECURSE "${OUTPUT_DIR}/${SYSTEM}")

endif()

file(CREATE_LINK ${OBJ} "${OUTPUT_DIR}/gamedb${SYSTEM}.o" SYMBOLIC)
