# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.22

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build

# Include any dependencies generated for this target.
include CMakeFiles/sysrepoctl.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include CMakeFiles/sysrepoctl.dir/compiler_depend.make

# Include the progress variables for this target.
include CMakeFiles/sysrepoctl.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/sysrepoctl.dir/flags.make

CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o: CMakeFiles/sysrepoctl.dir/flags.make
CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o: ../src/executables/sysrepoctl.c
CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o: CMakeFiles/sysrepoctl.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=//home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building C object CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -MD -MT CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o -MF CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o.d -o CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o -c //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/src/executables/sysrepoctl.c

CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.i"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/src/executables/sysrepoctl.c > CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.i

CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.s"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/src/executables/sysrepoctl.c -o CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.s

CMakeFiles/sysrepoctl.dir/compat/compat.c.o: CMakeFiles/sysrepoctl.dir/flags.make
CMakeFiles/sysrepoctl.dir/compat/compat.c.o: ../compat/compat.c
CMakeFiles/sysrepoctl.dir/compat/compat.c.o: CMakeFiles/sysrepoctl.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=//home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building C object CMakeFiles/sysrepoctl.dir/compat/compat.c.o"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -MD -MT CMakeFiles/sysrepoctl.dir/compat/compat.c.o -MF CMakeFiles/sysrepoctl.dir/compat/compat.c.o.d -o CMakeFiles/sysrepoctl.dir/compat/compat.c.o -c //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/compat/compat.c

CMakeFiles/sysrepoctl.dir/compat/compat.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/sysrepoctl.dir/compat/compat.c.i"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/compat/compat.c > CMakeFiles/sysrepoctl.dir/compat/compat.c.i

CMakeFiles/sysrepoctl.dir/compat/compat.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/sysrepoctl.dir/compat/compat.c.s"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/compat/compat.c -o CMakeFiles/sysrepoctl.dir/compat/compat.c.s

# Object files for target sysrepoctl
sysrepoctl_OBJECTS = \
"CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o" \
"CMakeFiles/sysrepoctl.dir/compat/compat.c.o"

# External object files for target sysrepoctl
sysrepoctl_EXTERNAL_OBJECTS =

sysrepoctl: CMakeFiles/sysrepoctl.dir/src/executables/sysrepoctl.c.o
sysrepoctl: CMakeFiles/sysrepoctl.dir/compat/compat.c.o
sysrepoctl: CMakeFiles/sysrepoctl.dir/build.make
sysrepoctl: libsysrepo.so.7.13.13
sysrepoctl: /usr/lib/x86_64-linux-gnu/librt.a
sysrepoctl: /usr/local/lib/libyang.so
sysrepoctl: CMakeFiles/sysrepoctl.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=//home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Linking C executable sysrepoctl"
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/sysrepoctl.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/sysrepoctl.dir/build: sysrepoctl
.PHONY : CMakeFiles/sysrepoctl.dir/build

CMakeFiles/sysrepoctl.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/sysrepoctl.dir/cmake_clean.cmake
.PHONY : CMakeFiles/sysrepoctl.dir/clean

CMakeFiles/sysrepoctl.dir/depend:
	cd //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build //home/hytec/gnb407/du_bin/intel/liboam/oam_du/cm/sysrepo/run/SysrepoInstaller/PKG/sysrepo/build/CMakeFiles/sysrepoctl.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/sysrepoctl.dir/depend

