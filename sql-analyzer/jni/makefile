# Define a variable for classpath
CLASS_PATH = ../target/classes/

# Define a virtual path for .class in the bin directory
vpath %.class $(CLASS_PATH)

all : sqlvalidator.dll

# $@ matches the target, $< matches the first dependancy
sqlvalidator.dll : SQLValidator.o
	gcc -shared -o $@ $<

# $@ matches the target, $< matches the first dependancy
SQLValidator.o : SQLValidator.c SQLValidator.h
	gcc -m64 -g -I"$(JAVA_HOME)\include" -I"$(JAVA_HOME)\include\win32" -I"$(CUBRID)\include" -c $< -o $@

# $* matches the target filename without the extension
SQLValidator.h : com/cubrid/validator/SQLValidator.class
	javah -classpath $(CLASS_PATH) com.cubrid.validator.$*

clean :
	rm com_cubrid_validator_SQLValidator.h SQLValidator.o sqlvalidator.dll