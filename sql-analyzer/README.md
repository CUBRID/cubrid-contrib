# SQL Analyzer
When migrating from another DBMS to CUBRID, it is necessary to check how compatible the query written with the syntax of another DBMS is with the syntax of CUBRID. There is no need to worry if the file is well organized for easy execution of the query, but the query stored in the Mapper XML file of MyBatis (iBATIS) cannot be easily executed. So, this tool was created to extract the dynamic query from the Mapper XML file of MyBatis (iBATIS) and execute it in CUBRID to easily check the compatibility.

# Requirements
- As of yet, it is only targeting Windows.
- Prepare the Mapper XML file of MyBatis (iBATIS) in the sqlmap (default) directory of the execution path.
- The versions of iBATIS and MyBatis are related below.
  - iBATIS 2.x
  - iBATIS 2.3 -> MyBatis 2.5
  - MyBatis 3.x
- CUBRID installation and JDBC connection
- Java 1.8 or higher
  - There is no need for an external library of java.
- Eclipse 4.6 (Neon) or higher

# Build in eclipse

### 1. Clone git repository in eclipse
- Window -> Preferences -> Java -> Installed JREs -> Add... -> Standard VM -> Directory... -> %JAVA\_HOME%/jre -> Finish -> OK
- Window -> Perspective -> Open Perspective -> Other... -> Git
- Git Repositories -> Clone a Git repository -> Next -> Next -> Finish
  - URI: https://github.com/CUBRID/cubrid-contrib.git
  - Branch: main
  - Directory: ...
- cubrid-contrib -> Working Tree -> sql-analyzer -> Import Projects... -> Finish
- Window -> Perspective -> Open Perspective -> Other... -> Java or Java EE

### 2. Install CDT (C/C++ Development Tooling) in Eclipse
- Package Explorer -> sql-analyzer -> jni -> SQLValidator.c -> [context menu] Open -> Show marketplace proposals and let me install them -> OK
- Eclipse Marketplace -> Eclipse C/C++ IDE CDT 9.2 (Neon.2) -> Install -> Confirm -> Finish
- Restart
- Window -> Perspective -> Open Perspective -> Other... -> C/C++

### 3. Build JNI (Java Native Interface)
- Build Targets -> sql-analyzer -> jni -> all -> [context menu] Build Target

### 4. Run
- Project Explorer -> sql-analyzer/src/com/cubrid/analyzer/SQLAnalyzer.java -> [context menu] Run As -> Run Configurations... -> Java Application -> SQLAnalyzer -> Arguments -> VM arguments: "-Djava.library.path=jni" -> Apply -> Run

### 5. Package
- Project Explorer -> sql-analyzer -> [context menu] Run As -> Maven build -> Goals: "clean package" -> Apply -> Run
- Project Explorer -> sql-analyzer/target -> sql-analyzer-2022.09.15-jar-with-dependencies.jar


# Implementation
- Parse Mapper XML file of MyBatis (iBATIS) using SAX (Simple API for XML) in java.
  - To extract dynamic queries, SAX (Simple API for XML) is better than DOM (Document Object Model).
- Summary information is output to the sumary.log (default) file in the execution path, and detailed information is output to the \*.log file with the same name in the path where the Mappter XML file of MyBatis (iBATIS) is located.

### 1. SQLAnalyzer (com/cubrid/analyzer/SQLAnalyzer.java)

| Function | Content |
|:---------|:--------|
| main | Create an instance of the SQLAnalyzer class and call the start function. |
| SQLAnalyzer | Create an instance of the DatabaseManager class and open a file to write summary information. |
| openSummary | Prints a message when the program starts. |
| appendDirectorySummary | Prints a message when a subdirectory search begins. |
| appendQueryNumber | Print the sequence number of the query file in the subdirectory. |
| appendQuerySummary | Print the relative path of the parsed query file in the subdirectory. |
| appendResultSummary | Print the result of the parsed query file and aggregate the result in the subdirectory. |
| appendSubTotalSummary | Print the aggregated result of the subdirectory. |
| appendTotalSummary | Prints all aggregated results. |
| closeSummary | Prints a message when the program ends. |
| start | Call the traverseDirectory function and the appendTotalSummary function. |
| **traverseDirectory** | The parse function is called for the \*.xml file while searching the subdirectories of the sqlmap (default) directory of the execution path. |
| **parse** | The analyze function of the SqlMapXMLParser class is called for the \*.xml file. |

```
start () -> traverseDirectory () -> isDirectory: true -+
                        | ^                            |
                        | |                            |
                        | +----------------------------+
                        |
                        +-> isFile: true --> parse () -> SqlMapXMLParser.analyze () ->
```

### 2. SqlMapXMLParser (com/cubrid/parser/SqlMapXMLParser.java)

| Function | Content |
|:---------|:--------|
| SqlMapXMLParser | Create instances of SAXParser class and XMLReader class. |
| **analyze** | Create an instance of the SqlXmlHandler class, add the LexicalHandler, and call the parse function of the SAXParser instance. |

```
SqlMapXMLParser.analyze () -> SAXParser.parse () -> SqlXmlHandler ->
```

### 3. SqlMapXMLTag (com/cubrid/parser/SqlMapXMLTag.java)
- This is a class to store attribute information of the XML tag searched for in the SqlXmlHandler class.

### 4. SqlXmlHandler (com/cubrid/parser/SqlXmlHandler.java)
- Extracts and executes dynamic queries from Mapper XML files in MyBatis (iBATIS).
- An instance of the SQLAnalyzerForCUBRID class is needed to print summary information.
- An instance of the DatabaseManager class is needed to prepare a query in CUBRID.
- The detailed result of the query file is printed in \*.log file in the same path.
- The opened tag is created as an instance of the SqlMapXMLTag class and stored in the stack.

| Function | Content |
|:---------|:--------|
| SqlXmlHandler | Create instances necessary for parsing the query file. |
| openLog | Prints a message when parsing a query file starts. |
| appendQueryId | Prints a message when parsing a query begins. |
| appendQuery | Prints a query when parsing a query begins. |
| appendResult | Call the closeLog function, and call the appendResultSummary function in the instance of the SQLAnalyzerForCUBRID class. |
| closeLog | Prints a message when parsing a query file ends. |
| startDocument |  Call the openLog function. |
| endDocument | Call the closeLog function and the appendTotalSummary function. |
| **startElement** | Creates an instance of the SqlMapXMLTag class and stores it on the stack. Parse only DML queries. |
| **characters** | It stores the content in the current tag on the stack if the content of the XML tag is not empty. |
| **endElement** | Complete the creation of the dynamic query and prepare the query in CUBRID. |
| startDTD | Overriding by LexicalHandler interface. It is empty. |
| endDTD | Overriding by LexicalHandler interface. It is empty. |
| startEntity | Overriding by LexicalHandler interface. It is empty. |
| endEntity | Overriding by LexicalHandler interface. It is empty. |
| startCDATA | Overriding by LexicalHandler interface. It is empty. |
| endCDATA | Overriding by LexicalHandler interface. It is empty. |
| comment | Overriding by LexicalHandler interface. It is empty. |
| **saveStartTag** | Save the attribute information of the XML tag in the instance of SqlMapXMLTag class. |
| pttrnMtchBlank | Pattern matching to handle the end of the buffer when processing the content of the XML tag |
| pttrnMtchSQL | Pattern matching for parameter binding. <br> - iBATIS: #value# (PreparedStatement), \\$value\\$ (Statement) <br> - MyBatis: #\{value\} (PreparedStatement), \$\{value\} (Statement) |

```
SqlXmlHandler -> startDocument ()
                   startElement ()
                     characters ()
                   endElement ()
                   startElement ()
                     characters ()
                     startElement ()
                       characters ()
                     endElement ()
                     startElement ()
                       startElement ()
                         characters ()
                       endElement ()
                     endElement ()
                   endElement ()
                   ...
                 endDocument ()
```

### 5. DatabaseManager (com/cubrid/database/DatabaseManager.java)

| Function | Content |
|:---------|:--------|
| DatabaseManager | Create an error buffer. Saves error messages that occur in the Mapper XML file. |
| **initConnect** | Call the connect function. Set onlyCheckValidation to false. |
| **initPseudoConnect** | Create instances of SQLValidator class. Set onlyCheckValidation to true. |
| **connect** | Connect to CUBRID using db.properties in the execution path. |
| close | Close the CUBRID connection. |
| getErrorBuffer | Returns the error buffer to store error messages. |
| resetErrorBuffer | Reset the error buffer. |
| **checkQuery** | If onlyCheckValidation is true, the validateQuery function is called, and if it is false, the prepareQuery function is called. |
| **prepareQuery** | Execute the query by wrapping it in PREPARE statement. |
| **validateQuery** | Using JNI, only the syntax check is executed, not the semantic check. |

```
initPseudoConnect () -> checkQuery () -> onlyCheckValidation : true -> validateQuery () -> SQLValidator.validateSQL () ->

initConnect () -> checkQuery () -> onlyCheckValidation : false -> prepareQuery () ->
```

### 6. SQLValidator (com/cubrid/validator/SQLValidator.java)
- Using JNI, only the syntax check is executed, not the semantic check.
- The following files are required.

```
$CUBRID/include/dbi.h           /* when building */
$CUBRID/bin/cubridcs.dll        /* when running */
```

- The following files are used or created when building.

```
/* -Djava.library.path=jni */

jni/makefile
jni/com_cubrid_validator_SQLValidator.h
jni/SQLValidator.c
jni/SQLValidator.o
jni/sqlvalidator.dll
```

# Usage

```
# Windows

# Copy the Mapper XML files of MyBatis (iBATIS) to the sqlmap directory.

java -Djava.library.path=jni -jar sql-analyzer-2022.09.15-jar-with-dependencies.jar

type summary.log | more

# Check the detailed log file of the Mapper XML files of MyBatis (iBATIS) in the sqlmap directory.
```
