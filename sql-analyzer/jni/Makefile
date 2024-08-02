CFLAGS += -fPIC -I"$(JAVA_HOME)/include" -I"$(JAVA_HOME)/include/win32" -I"$(CUBRID)/include"
LDFLAGS += -shared

SQLValidator.class :
	javac ../src/main/java/org/cubrid/validator/SQLValidator.java

sqlvalidator.h : SQLValidator.class
	javah -cp ../target/classes/ org.cubrid.validator.SQLValidator

# $@ matches the target, $< matches the first dependancy
sqlvalidator.o : sqlvalidator.c sqlvalidator.h
	gcc -c $(CFLAGS) -o $@ $<

# $@ matches the target, $< matches the first dependancy
sqlvalidator.dll : sqlvalidator.o
	gcc $(LDFLAGS) -o $@ $<
	
all : clean sqlvalidator.dll

clean :
	rm -f org_cubrid_validator_SQLValidator.h
	rm -f sqlvalidator.o
	rm -f sqlvalidator.dll