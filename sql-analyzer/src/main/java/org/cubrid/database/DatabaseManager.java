package org.cubrid.database;

import java.io.FileReader;
import java.io.IOException;
import java.io.Reader;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.Properties;

import org.cubrid.validator.SQLValidator;

public class DatabaseManager {
	private final String NO_ERROR = "NO_ERROR";

	public final static String QUERY_ERROR_FILTER = ".*The\\p{Space}constraint\\p{Space}of\\p{Space}the\\p{Space}foreign\\p{Space}key.*"
			+ "|.*restricted\\p{Space}by\\p{Space}the\\p{Space}foreign\\p{Space}key.*"
			+ "|.*one\\p{Space}or\\p{Space}more\\p{Space}unique\\p{Space}constraint\\p{Space}violations.*"
			+ "|.*SQL\\p{Space}statement\\p{Space}violated\\p{Space}NOT\\p{Space}NULL\\p{Space}constraint.*"
			+ "|.*Unknown\\p{Space}class.*";

	public final static String QUERY_ERROR_FOREIGNKEY = ".*The\\p{Space}constraint\\p{Space}of\\p{Space}the\\p{Space}foreign\\p{Space}key.*|.*restricted\\p{Space}by\\p{Space}the\\p{Space}foreign\\p{Space}key.*";
	public final static String QUERY_ERROR_UNIQUE = "|.*one\\p{Space}or\\p{Space}more\\p{Space}unique\\p{Space}constraint\\p{Space}violations.*";
	public final static String QUERY_ERROR_NOTNULL = "|.*SQL\\p{Space}statement\\p{Space}violated\\p{Space}NOT\\p{Space}NULL\\p{Space}constraint.*";
	public final static String QUERY_ERROR_FORMAT = ".*Invalid\\p{Space}format.*";
	public final static String TODATE_ERROR_FORMAT = ".*YYYYMMDDHH24MISS.*";
	public final static String UNKNOWN_CLASS_ERROR_FORMAT = ".*\nUnknown\\p{Space}class.*";

	private Connection connection = null;
	private SQLValidator validator = null;
	private boolean onlyCheckValidation = false;
	private StringBuilder errorBuffer = null;

	public DatabaseManager() {
		errorBuffer = new StringBuilder();
	}

	public void initConnect() {
		this.connect();
	}

	public void initPseudoConnect() {
		validator = new SQLValidator();
		onlyCheckValidation = true;
	}

	private void connect() {
		Properties prop = new Properties();

		try {
			Reader reader = new FileReader("db.properties");
			prop.load(reader);
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		String url = prop.getProperty("url");
		String user = prop.getProperty("user");
		String password = prop.getProperty("password");

		try {
			Class.forName("cubrid.jdbc.driver.CUBRIDDriver");
			connection = DriverManager.getConnection(url, user, password);
			connection.setAutoCommit(true);
		} catch (Exception e) {
			System.err.println(e.getMessage());
		}
		
		onlyCheckValidation = false;
	}

	public Connection getConnection() {
		return this.connection;
	}
	
	public void close() {
		try {
			connection.close();
		} catch (SQLException e) {
			System.err.println(e.getMessage());
		}
	}

	public StringBuilder getErrorBuffer() {
		return errorBuffer;
	}

	public void resetErrorBuffer() {
		errorBuffer.setLength(0);
	}

	public String checkQuery(String query) {
		if (onlyCheckValidation) {
			return validateQuery(query);
		} else {
			return prepareQuery(query);
		}
	}

	public String prepareQuery(String query) {
		String prepareQuery = null;
		prepareQuery = query.replaceAll("'", "''");
		prepareQuery = "PREPARE q FROM '" + prepareQuery + "'";

		try (PreparedStatement pstmt = connection.prepareStatement(prepareQuery)) {
			pstmt.executeUpdate();
		} catch (Exception e) {
			// System.err.println(e.getMessage());
			return e.getMessage();
		}

		return "NO_ERROR";
	}

	public String validateQuery(String query) {
		return validator.validateSQL(query);
	}
}