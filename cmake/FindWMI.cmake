# FindWMI.cmake - Locate WMI libraries on Windows
# Sets WMI_FOUND, WMI_LIBRARIES

if(WIN32)
    set(WMI_LIBRARIES wbemuuid ole32 oleaut32 setupapi)
    set(WMI_FOUND TRUE)
else()
    set(WMI_FOUND FALSE)
endif()
