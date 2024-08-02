package org.cubrid.parser;

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.Stack;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.cubrid.analyzer.SQLAnalyzer;
import org.cubrid.database.DatabaseManager;
import org.xml.sax.Attributes;
import org.xml.sax.SAXException;
import org.xml.sax.ext.DefaultHandler2;
import org.xml.sax.ext.LexicalHandler;

import ognl.OgnlHelper;

public class SqlMapHandler extends DefaultHandler2 implements LexicalHandler {
	private final String BIND_VARIABLE = "?";
	private final String SPACE_SEPARATOR = " ";

	private final String NO_ERROR = "NO_ERROR";

	private final String TAG_SQLMAP = "SQLMAP";
	private final String TAG_MAPPER = "MAPPER";

	private final String TAG_SELECT = "SELECT";
	private final String TAG_INSERT = "INSERT";
	private final String TAG_UPDATE = "UPDATE";
	private final String TAG_DELETE = "DELETE";

	private final String TAG_DML = TAG_SELECT + "|" + TAG_INSERT + "|" + TAG_UPDATE + "|" + TAG_DELETE;

	private final String TAG_SELECTKEY = "SELECTKEY";
	private final String TAG_SET = "SET";
	private final String TAG_DYNAMIC = "DYNAMIC";
	private final String TAG_ITERATE = "ITERATE";
	private final String TAG_ISNOTEMPTY = "ISNOTEMPTY";
	private final String TAG_ISEMPTY = "ISEMPTY";
	private final String TAG_ISNOTEQUAL = "ISNOTEQUAL";
	private final String TAG_ISEQUAL = "ISEQUAL";
	private final String TAG_ISGREATEREQUAL = "ISGREATEREQUAL";
	private final String TAG_ISLESSTHAN = "ISLESSTHAN";
	private final String TAG_CHOOSE = "CHOOSE";
	private final String TAG_WHEN = "WHEN";
	private final String TAG_OTHERWISE = "OTHERWISE";
	private final String TAG_IF = "IF";

	private final String TAG_COMMAND = TAG_SELECTKEY + "|" + TAG_SET + "|" + TAG_DYNAMIC + "|" + TAG_ITERATE + "|"
			+ TAG_ISNOTEMPTY + "|" + TAG_ISEMPTY + "|" + TAG_ISNOTEQUAL + "|" + TAG_ISEQUAL + "|" + TAG_ISGREATEREQUAL
			+ "|" + TAG_ISLESSTHAN;

	private final String TAG_RESULT = "RESULT";
	private final String TAG_RESULTMAP = "RESULTMAP";
	private final String TAG_PROPERTIES = "PROPERTIES";
	private final String TAG_COMMENT = "COMMENT";
	private final String TAG_ENTRY = "ENTRY";

	private final String TAG_OTHER = TAG_RESULT + "|" + TAG_RESULTMAP + "|" + TAG_PROPERTIES + "|" + TAG_COMMENT + "|"
			+ TAG_ENTRY;

	private final String TAG_KNOWN = TAG_MAPPER + "|" + TAG_DML + "|" + TAG_COMMAND + "|" + TAG_OTHER;

	private final String SQL_MAP_CONFIG_DTD = "http://ibatis.apache.org/dtd/sql-map-config-2.dtd";
	private final String SQL_MAP_DTD = "http://ibatis.apache.org/dtd/sql-map-2.dtd";

	private final String MAPPER_CONFIG_DTD = "http://mybatis.org/dtd/mybatis-3-config.dtd";
	private final String MAPPER_DTD = "http://mybatis.org/dtd/mybatis-3-mapper.dtd";

	private OgnlHelper ognlHelper = null;
	private Map<String, Object> parameterMap = null;

	private Map<String, String> mapDocType = null;
	private Map<String, String> mapIsEmpty = null;
	private Map<String, String> mapIsEqual = null;
	private Map<String, String> mapIsGreaterLess = null;

	private Stack<SqlMapTag> stackReadTag = null;

	private int count = 0;
	private int errorCount = 0;

	private DatabaseManager databaseManager = null;

	private SQLAnalyzer sqlAnalyzer = null;

	private String filePath = null;
	private String fileName = null;
	private String logFilePath = null;

	private BufferedWriter writerLog = null;

	public SqlMapHandler(SQLAnalyzer sqlAnalyzer, DatabaseManager databaseManager, String filePath, String fileName) {
		this.sqlAnalyzer = sqlAnalyzer;
		this.databaseManager = databaseManager;
		this.filePath = filePath;
		this.fileName = fileName;

		logFilePath = this.filePath.substring(0, filePath.lastIndexOf(".")) + ".log";

		try {
			writerLog = new BufferedWriter(new FileWriter(logFilePath));
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
		
		ognlHelper = new OgnlHelper();
		parameterMap= new HashMap<String, Object>();

		mapDocType = new HashMap<String, String>();
		
		/* iBATIS 2.x
		 *   - https://github.com/mybatis/ibatis-2/tree/master/src/main/resources/com/ibatis/sqlmap/engine/builder/xml
		 */
		// mapDocType.put("http://www.ibatis.com/dtd/sql-map-config-2.dtd".toUpperCase(), SQL_MAP_CONFIG_DTD);
		mapDocType.put("http://www.ibatis.com/dtd/sql-map-2.dtd".toUpperCase(), SQL_MAP_DTD);
		
		/* www.ibatis.com -> ibatis.apache.org */
		// mapDocType.put("-//ibatis.apache.org//DTD SQL Map Config 2.0//EN".toUpperCase(), SQL_MAP_CONFIG_DTD);
		mapDocType.put("-//ibatis.apache.org//DTD SQL Map 2.0//EN".toUpperCase(), SQL_MAP_DTD);
		
		/* iBATIS 2.3 -> MyBatis 2.5 */
														
		/* MyBatis 3.x
		 *   - https://github.com/mybatis/mybatis-3/tree/master/src/main/java/org/apache/ibatis/builder/xml
		 */
		// mapDocType.put("-//mybatis.org//DTD Config 3.0//EN".toUpperCase(), MAPPER_CONFIG_DTD);
		mapDocType.put("-//mybatis.org//DTD Mapper 3.0//EN".toUpperCase(), MAPPER_DTD);

		mapIsEmpty = new HashMap<String, String>();
		mapIsEqual = new HashMap<String, String>();
		mapIsGreaterLess = new HashMap<String, String>();

		stackReadTag = new Stack<SqlMapTag>();

		count = 0;
		errorCount = 0;
	}

	private void openLog() {
		try {
			writerLog.write("/* Open File: " + fileName + " */");
			writerLog.append(System.getProperty("line.separator"));
			writerLog.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}

	private void appendQueryId(String query, String queryId) {
		count++;

		try {
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("================================================================================");
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("Number   : " + count);
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("Query    : " + query);
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("Query Id : " + queryId);
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("================================================================================");
			writerLog.append(System.getProperty("line.separator"));
			writerLog.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}

	private void appendQuery(String query) {
		try {
			writerLog.append(query + System.getProperty("line.separator"));
			writerLog.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}

	private void appendResult(String error) {
		try {
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("--------------------------------------------------------------------------------");
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append(error);
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("--------------------------------------------------------------------------------");
			writerLog.append(System.getProperty("line.separator"));
			writerLog.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}

	private void closeLog() {
		try {
			writerLog.append(System.getProperty("line.separator"));
			writerLog.append("/* Close File: " + fileName + " */");
			writerLog.append(System.getProperty("line.separator"));
			writerLog.flush();
			writerLog.close();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}

	@Override
	public void startDocument() throws SAXException {
		openLog();
	}

	@Override
	public void endDocument() throws SAXException {
		closeLog();

		sqlAnalyzer.appendResultSummary(count, count - errorCount, errorCount);
	}

	@Override
	public void startElement(String uri, String localName, String qName, Attributes attributes) throws SAXException {
		SqlMapTag currentTag = new SqlMapTag();
		saveStartTag(currentTag, localName, attributes);

		if (currentTag.getName().toUpperCase().matches(TAG_DML)) {
			appendQueryId(currentTag.getName(), currentTag.getId());
			parameterMap.clear();
		}
	}

	@Override
	public void characters(char[] ch, int start, int length) throws SAXException {
		String contents = new String(ch, start, length);
		contents = contents.replaceAll("\t", "    ");

		String checkEmpty = contents.trim().replaceAll("\\s", "");

		if (!("".equals(checkEmpty))) {
			SqlMapTag currentTag = stackReadTag.peek();
			currentTag.addContents(contents);
		}
	}

	@Override
	public void endElement(String uri, String localName, String qName) throws SAXException {
		SqlMapTag currentTag = stackReadTag.pop();
		SqlMapTag beforeTag = null;
		String bindContents = null;

		if (stackReadTag.size() > 0) {
			beforeTag = stackReadTag.peek();
		}

		switch (currentTag.getName().toUpperCase()) {
		case TAG_SELECT:
		case TAG_INSERT:
		case TAG_UPDATE:
		case TAG_DELETE:
			String query = pttrnMtchSQL(currentTag.getContents()) + ";";
			String result = null;
			appendQuery(query);
			result = databaseManager.checkQuery(query);
			if (!NO_ERROR.equals(result)) {
				errorCount++;

				/* debug */
				StringBuilder errorBuffer = databaseManager.getErrorBuffer();
				if (errorBuffer.length() == 0) {
					errorBuffer
							.append("--------------------------------------------------------------------------------");
				}
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append("[" + currentTag.getName().toUpperCase() + "] ID: " + currentTag.getId());
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append(result);
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append("--------------------------------------------------------------------------------");
				/**/
			}
			/* debug */
			StringBuilder ognlHelperErrorBuffer = ognlHelper.getErrorBuffer();
			if (ognlHelperErrorBuffer.length() != 0) {
				StringBuilder errorBuffer = databaseManager.getErrorBuffer();
				if (errorBuffer.length() == 0) {
					errorBuffer
							.append("--------------------------------------------------------------------------------");
				}
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append("[" + currentTag.getName().toUpperCase() + "] ID: " + currentTag.getId());
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append(ognlHelperErrorBuffer.toString());
				errorBuffer.append(System.getProperty("line.separator"));
				errorBuffer.append("--------------------------------------------------------------------------------");

				ognlHelper.resetErrorBuffer();
			}
			/**/

			appendResult(result);
			break;

		default:
			break;
		}

		if (!("".equals(currentTag.getContents().trim()))) {
			switch (currentTag.getName().toUpperCase()) {
			case TAG_SELECTKEY:
				bindContents = pttrnMtchSQL(currentTag.getContents());
				appendQuery(new String(bindContents + ";"));
				break;

			case TAG_SET:
				bindContents = pttrnMtchSQL(currentTag.getContents());

				beforeTag.addContents(" SET ");

				beforeTag.addContents(bindContents);

				beforeTag.addContents(SPACE_SEPARATOR);
				break;

			case TAG_DYNAMIC:
				bindContents = pttrnMtchSQL(currentTag.getContents());

				beforeTag.addContents(SPACE_SEPARATOR);

				if (currentTag.getPrepend() != null) {
					beforeTag.addContents(currentTag.getPrepend());
					beforeTag.addContents(SPACE_SEPARATOR);
				}

				if (currentTag.getOpen() != null) {
					beforeTag.addContents(currentTag.getOpen());
				}

				beforeTag.addContents(bindContents);

				if (currentTag.getClose() != null) {
					beforeTag.addContents(currentTag.getClose());
				}

				beforeTag.addContents(SPACE_SEPARATOR);
				break;

			case TAG_ITERATE:
				bindContents = pttrnMtchSQL(currentTag.getContents());

				if (currentTag.getPrepend() != null) {
					beforeTag.addContents(currentTag.getPrepend());
					beforeTag.addContents(SPACE_SEPARATOR);
				}

				if (currentTag.getOpen() != null) {
					beforeTag.addContents(currentTag.getOpen());
				}

				beforeTag.addContents(bindContents);
				beforeTag.addContents(SPACE_SEPARATOR).addContents(currentTag.getConjunction())
						.addContents(SPACE_SEPARATOR);
				beforeTag.addContents(bindContents);
				beforeTag.addContents(SPACE_SEPARATOR).addContents(currentTag.getConjunction())
						.addContents(SPACE_SEPARATOR);
				beforeTag.addContents(bindContents);
				beforeTag.addContents(SPACE_SEPARATOR).addContents(currentTag.getConjunction())
						.addContents(SPACE_SEPARATOR);
				beforeTag.addContents(bindContents);
				beforeTag.addContents(SPACE_SEPARATOR).addContents(currentTag.getConjunction())
						.addContents(SPACE_SEPARATOR);
				beforeTag.addContents(bindContents);

				if (currentTag.getClose() != null) {
					beforeTag.addContents(currentTag.getClose());
				}

				beforeTag.addContents(SPACE_SEPARATOR);
				break;

			case TAG_ISNOTEMPTY:
				if (!(mapIsEmpty.containsKey(currentTag.getProperty()))) {
					mapIsEmpty.put(currentTag.getProperty(), "NOTEMPTY");
				}

				if ("NOTEMPTY".equals(mapIsEmpty.get(currentTag.getProperty()).toUpperCase())) {
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() == null)) {
						beforeTag.addContents(" " + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() != null)) {
						beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() == null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getContents() + " ");
						}
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() != null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getOpen()
									+ currentTag.getContents() + " ");
						}
					}
				}
				break;

			case TAG_ISEMPTY:
				if (!(mapIsEmpty.containsKey(currentTag.getProperty()))) {
					mapIsEmpty.put(currentTag.getProperty(), "NOTEMPTY");
				}
				if ("EMPTY".equals(mapIsEmpty.get(currentTag.getProperty()).toUpperCase())) {
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() == null)) {
						beforeTag.addContents(" " + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() != null)) {
						beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() == null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getContents() + " ");
						}
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() != null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getOpen()
									+ currentTag.getContents() + " ");
						}
					}
				}
				break;

			case TAG_ISNOTEQUAL:
				if (!(mapIsEqual.containsKey(currentTag.getProperty()))) {
					mapIsEqual.put(currentTag.getProperty(), currentTag.getCompareValue());
				}
				if (!(currentTag.getCompareValue().equals(mapIsEqual.get(currentTag.getProperty())))) {
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() == null)) {
						beforeTag.addContents(" " + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() != null)) {
						beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() == null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getContents() + " ");
						}
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() != null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getOpen()
									+ currentTag.getContents() + " ");
						}
					}
				}
				break;

			case TAG_ISEQUAL:
				if (!(mapIsEqual.containsKey(currentTag.getProperty()))) {
					mapIsEqual.put(currentTag.getProperty(), currentTag.getCompareValue());
				}
				if (currentTag.getCompareValue().equals(mapIsEqual.get(currentTag.getProperty()))) {
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() == null)) {
						beforeTag.addContents(" " + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() == null) && (currentTag.getOpen() != null)) {
						beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() == null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getContents() + " ");
						}
					}
					if ((currentTag.getPrepend() != null) && (currentTag.getOpen() != null)) {
						if ((beforeTag.getPrepend() != null) && (beforeTag.getContents().length() == 0)
								&& (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getOpen() + currentTag.getContents() + " ");
						} else if ((beforeTag.getContents().length() > 0) && (currentTag.getContents().length() > 0)) {
							beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getOpen()
									+ currentTag.getContents() + " ");
						}
					}
				}
				break;

			case TAG_ISGREATEREQUAL:
				if (!(mapIsGreaterLess.containsKey(currentTag.getProperty()))) {
					mapIsGreaterLess.put(currentTag.getProperty(),
							String.valueOf(Integer.parseInt(currentTag.getCompareValue()) + 1));
				}
				if (Integer.parseInt(mapIsGreaterLess.get(currentTag.getProperty())) >= Integer
						.parseInt(currentTag.getCompareValue())) {
					if (currentTag.getPrepend() == null) {
						beforeTag.addContents(" " + currentTag.getContents() + " ");
					} else if (currentTag.getPrepend() != null) {
						beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getContents() + " ");
					}
				}
				break;

			case TAG_ISLESSTHAN:
				if (!(mapIsGreaterLess.containsKey(currentTag.getProperty()))) {
					mapIsGreaterLess.put(currentTag.getProperty(),
							String.valueOf(Integer.parseInt(currentTag.getCompareValue()) + 1));
				}
				if (Integer.parseInt(mapIsGreaterLess.get(currentTag.getProperty())) < Integer
						.parseInt(currentTag.getCompareValue())) {
					if (currentTag.getPrepend() == null) {
						beforeTag.addContents(" " + currentTag.getContents() + " ");
					} else if (currentTag.getPrepend() != null) {
						beforeTag.addContents(" " + currentTag.getPrepend() + " " + currentTag.getContents() + " ");
					}
				}
				break;

			case TAG_CHOOSE:
				beforeTag.addContents(currentTag.getContents());
				break;

			case TAG_WHEN:
				if (TAG_CHOOSE.equals(beforeTag.getName().toUpperCase()) && beforeTag.getContents().isEmpty()) {
					beforeTag.addContents(" " + pttrnMtchSQL(currentTag.getContents()) + " ");
				}
				break;

			case TAG_OTHERWISE:
				break;

			case TAG_IF:
				if (ognlHelper.getValue(currentTag.getTest(), parameterMap)) {
					beforeTag.addContents(" " + pttrnMtchSQL(currentTag.getContents()) + " ");
				}
				break;

			default:
				break;
			}
		}
	}

	@Override
	public void startDTD(String name, String publicId, String systemId) throws SAXException {
	}

	@Override
	public void endDTD() throws SAXException {
	}

	@Override
	public void startEntity(String name) throws SAXException {
	}

	@Override
	public void endEntity(String name) throws SAXException {
	}

	@Override
	public void startCDATA() throws SAXException {
	}

	@Override
	public void endCDATA() throws SAXException {
	}

	@Override
	public void comment(char[] ch, int start, int length) throws SAXException {
	}

	private void saveStartTag(SqlMapTag tag, String localName, Attributes attributes) {
		tag.setName(localName);

		if (attributes.getValue("id") != null) {
			tag.setId(attributes.getValue("id"));
		}

		if (attributes.getValue("property") != null) {
			tag.setProperty(attributes.getValue("property"));
		}

		if (attributes.getValue("prepend") != null) {
			tag.setPrepend(attributes.getValue("prepend"));
		}

		if (attributes.getValue("compareValue") != null) {
			tag.setCompareValue(attributes.getValue("compareValue"));
		}

		if (attributes.getValue("open") != null) {
			tag.setOpen(attributes.getValue("open"));
		}

		if (attributes.getValue("close") != null) {
			tag.setClose(attributes.getValue("close"));
		}

		if (attributes.getValue("conjunction") != null) {
			tag.setConjunction(attributes.getValue("conjunction"));
		}

		if (attributes.getValue("test") != null) {
			tag.setTest(attributes.getValue("test"));
		}

		stackReadTag.push(tag);
	}

	public String pttrnMtchBlank(Object sql) {
		String pttrn_str1 = "[) ]+\\p{Alnum}+$";

		Pattern pattern = null;

		String query = String.valueOf(sql);

		pattern = Pattern.compile(pttrn_str1);
		Matcher matcher = pattern.matcher(query);
		while (matcher.find()) {
			return query + " ";
		}

		return query;
	}

	public String pttrnMtchSQL(Object sql) {
		/* iBATIS */
		String pttrn_str1 = "[$][^$}]+[$]";
		String pttrn_str2 = "[#][^#}]+[#]";

		/* MyBatis */
		String pttrn_str3 = "[#{][^#}]+[}]";
		String pttrn_str4 = "[${][^$}]+[}]";

		Pattern pattern = null;

		String query = String.valueOf(sql);

		pattern = Pattern.compile(pttrn_str1);
		Matcher matcher = pattern.matcher(query);
		while (matcher.find()) {
			query = matcher.replaceAll("?");
		}

		pattern = Pattern.compile(pttrn_str2);
		matcher = pattern.matcher(query);
		while (matcher.find()) {
			query = matcher.replaceAll("?");
		}

		pattern = Pattern.compile(pttrn_str3);
		matcher = pattern.matcher(query);
		while (matcher.find()) {
			query = matcher.replaceAll("?");
		}

		pattern = Pattern.compile(pttrn_str4);
		matcher = pattern.matcher(query);
		while (matcher.find()) {
			query = matcher.replaceAll("?");
		}

		return query;
	}
}