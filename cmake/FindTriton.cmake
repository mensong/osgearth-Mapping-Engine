# Find TRITON toolkit
# This module defines
# TRITON_FOUND
# TRITON_INCLUDE_DIR
# On windows:
#   TRITON_LIBRARY_DEBUG
#   TRITON_LIBRARY_RELEASE
# On other platforms
#   TRITON_LIBRARY
#

SET(TRITON_DIR "" CACHE PATH "Location of Triton SDK")

IF (MSVC90)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc9/x64")
	ELSE()
		SET(TRITON_ARCH "vc9/win32")
	ENDIF()
ENDIF()

IF (MSVC80)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc8/x64")
	ELSE()
		SET(TRITON_ARCH "vc8/win32")
	ENDIF()
ENDIF()

IF (MSVC10)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc10/x64")
	ELSE()
		SET(TRITON_ARCH "vc10/win32")
	ENDIF()
ENDIF()

IF (MSVC11)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc11/x64")
	ELSE()
		SET(TRITON_ARCH "vc11/win32")
	ENDIF()
ENDIF()

IF (MSVC12)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc12/x64")
	ELSE()
		SET(TRITON_ARCH "vc12/win32")
	ENDIF()
ENDIF()

IF (MSVC14)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc14/x64")
	ELSE()
		SET(TRITON_ARCH "vc14/win32")
	ENDIF()
ENDIF()

IF (MSVC15)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc15/x64")
	ELSE()
		SET(TRITON_ARCH "vc15/win32")
	ENDIF()
ENDIF()

IF (MSVC16)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc16/x64")
	ELSE()
		SET(TRITON_ARCH "vc16/win32")
	ENDIF()
ENDIF()

IF (MSVC17)
	IF (CMAKE_CL_64)
		SET(TRITON_ARCH "vc17/x64")
	ELSE()
		SET(TRITON_ARCH "vc17/win32")
	ENDIF()
ENDIF()

IF (MSVC71)
	SET(TRITON_ARCH "vc7")
ENDIF()

IF (MSVC60)
	SET(TRITON_ARCH "vc6")
ENDIF()

IF (UNIX)
	SET(TRITON_ARCH "linux")
ENDIF()

FIND_PATH(TRITON_INCLUDE_DIR Triton.h
    "${TRITON_DIR}/Public Headers"
    "$ENV{TRITON_PATH}/Public Headers"
    $ENV{TRITON_PATH}
    ${TRITON_DIR}/include
    $ENV{TRITON_DIR}/include
    $ENV{TRITON_DIR}
    /usr/local/include
    /usr/include
    /sw/include # Fink
    /opt/local/include # DarwinPorts
    /opt/csw/include # Blastwave
    /opt/include
    /usr/freeware/include
    "C:/Triton SDK/Public Headers"
)

MACRO(FIND_TRITON_LIBRARY MYLIBRARY MYLIBRARYNAME)

    FIND_LIBRARY(${MYLIBRARY}
    NAMES ${MYLIBRARYNAME}
    PATHS
		${TRITON_DIR}/lib
		$ENV{TRITON_DIR}/lib
		$ENV{TRITON_DIR}
		$ENV{TRITON_PATH}/lib
		/usr/local/lib
		/usr/lib
		/sw/lib
		/opt/local/lib
		/opt/csw/lib
		/opt/lib
		/usr/freeware/lib64
        "C:/Triton SDK/lib"
	PATH_SUFFIXES
		${TRITON_ARCH}
    )

ENDMACRO()


FIND_TRITON_LIBRARY(TRITON_LIBRARY       "Triton-MT-DLL;Triton")
FIND_TRITON_LIBRARY(TRITON_LIBRARY_DEBUG "Triton-MTD-DLL;Triton")

SET(TRITON_FOUND FALSE)
IF (TRITON_INCLUDE_DIR AND TRITON_LIBRARY AND TRITON_LIBRARY_DEBUG)
   SET(TRITON_FOUND TRUE)
   add_library(OE::TRITON SHARED IMPORTED)
   set_target_properties(OE::TRITON PROPERTIES
       INTERFACE_INCLUDE_DIRECTORIES "${TRITON_INCLUDE_DIR}"
       INTERFACE_LINK_LIBRARIES "OpenGL::GL;OpenGL::GLU"
   )
   if(WIN32)
       set_target_properties(OE::TRITON PROPERTIES
           IMPORTED_IMPLIB "${TRITON_LIBRARY}"
           IMPORTED_IMPLIB_DEBUG "${TRITON_LIBRARY_DEBUG}"
       )
   else()
       set_target_properties(OE::TRITON PROPERTIES
           IMPORTED_LOCATION "${TRITON_LIBRARY}"
           IMPORTED_LOCATION_DEBUG "${TRITON_LIBRARY_DEBUG}"
       )
   endif()
   #SET(TRITON_LIBRARY debug ${TRITON_LIBRARY_DEBUG} optimized ${TRITON_LIBRARY_RELEASE})
ENDIF()

IF (TRITON_FOUND)
   IF (NOT TRITON_FIND_QUIETLY)
      MESSAGE(STATUS "Found Triton: ${TRITON_LIBRARY}")
   ENDIF()
ELSE()
   IF (TRITON_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find Triton")
   ENDIF()
ENDIF()
