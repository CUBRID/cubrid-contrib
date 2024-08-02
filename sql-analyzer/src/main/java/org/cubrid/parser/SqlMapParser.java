package org.cubrid.parser;

import java.io.IOException;

import javax.xml.parsers.ParserConfigurationException;
import javax.xml.parsers.SAXParser;
import javax.xml.parsers.SAXParserFactory;

import org.cubrid.analyzer.SQLAnalyzer;
import org.cubrid.database.DatabaseManager;
import org.xml.sax.SAXException;
import org.xml.sax.SAXNotRecognizedException;
import org.xml.sax.SAXNotSupportedException;
import org.xml.sax.XMLReader;

public class SqlMapParser {
	private SAXParserFactory saxParserFactory = null;
	private SAXParser saxParser = null;
	private XMLReader xmlReader = null;

	public SqlMapParser() {
		saxParserFactory = SAXParserFactory.newInstance();
		saxParserFactory.setNamespaceAware(true);
		saxParserFactory.setValidating(false);

		try {
			saxParserFactory.setFeature("http://apache.org/xml/features/nonvalidating/load-external-dtd", false);
		} catch (SAXNotRecognizedException e) {
			System.err.println(e.getMessage());
		} catch (SAXNotSupportedException e) {
			System.err.println(e.getMessage());
		} catch (ParserConfigurationException e) {
			System.err.println(e.getMessage());
		}

		try {
			saxParser = saxParserFactory.newSAXParser();
		} catch (ParserConfigurationException e) {
			System.err.println(e.getMessage());
		} catch (SAXException e) {
			System.err.println(e.getMessage());
		}

		try {
			xmlReader = saxParser.getXMLReader();
		} catch (SAXException e) {
			System.err.println(e.getMessage());
		}
	}

	public void analyze(SQLAnalyzer sqlAnalyzer, DatabaseManager databaseManager, String filePath, String fileName) {
		SqlMapHandler sqlXmlHandler = new SqlMapHandler(sqlAnalyzer, databaseManager, filePath, fileName);

		try {
			xmlReader.setProperty("http://xml.org/sax/properties/lexical-handler", sqlXmlHandler);
		} catch (SAXNotRecognizedException e) {
			System.err.println(e.getMessage());
		} catch (SAXNotSupportedException e) {
			System.err.println(e.getMessage());
		}

		try {
			saxParser.parse(filePath, sqlXmlHandler);
		} catch (SAXException e) {
			System.err.println(e.getMessage());
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}
}
