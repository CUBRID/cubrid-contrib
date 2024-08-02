package org.cubrid.analyzer;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.net.URI;

import org.cubrid.database.DatabaseManager;
import org.cubrid.parser.SqlMapParser;

import com.beust.jcommander.JCommander;

public class SQLAnalyzer {
	private final String SUMMARY_FILE_NAME = "summary.log";
	private final String SQLMAP_PATH = "sqlmap";

	private boolean syntaxOnly = false;

	private String resultFilePath = null;
	private File resultFile = null;

	private URI rootPath = null;
	private BufferedWriter writerSummary = null;

	private int count = 0;
	private int totalCount = 0;
	private int totalSuccessCount = 0;
	private int totalErrorCount = 0;
	private int subTotalCount = 0;
	private int subTotalSuccessCount = 0;
	private int subTotalErrorCount = 0;

	private boolean isBeforeFile = false;

	private DatabaseManager databaseManager = null;

	public static void main(String[] args) {
		SQLAnalyzerArgs sqlAnalyzerArgs = new SQLAnalyzerArgs();

		JCommander jCommander = JCommander.newBuilder().addObject(sqlAnalyzerArgs).build();
		jCommander.parse(args);

		if (sqlAnalyzerArgs.getHelp()) {
			jCommander.usage();
			return;
		}

		SQLAnalyzer sqlAnalyzer = new SQLAnalyzer(sqlAnalyzerArgs.getSyntaxOnly());
		sqlAnalyzer.start();
	}

	public SQLAnalyzer(boolean syntaxOnly) {
		databaseManager = new DatabaseManager();

		if (syntaxOnly) {
			databaseManager.initPseudoConnect();
		} else {
			databaseManager.initConnect();
		}

		try {
			rootPath = new File(".").toURI();
			writerSummary = new BufferedWriter(new FileWriter(SUMMARY_FILE_NAME));
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}
	}

	private void openSummary() {
		try {
			writerSummary.write("Start SQL Analyzer.");
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.write("- T: Total");
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.write("- S: Success");
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.write("- E: Error");
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.println("Start SQL Analyzer.");
		System.out.println("- T: Total");
		System.out.println("- S: Success");
		System.out.println("- E: Error");
	}

	private void appendDirectorySummary(String filePath) {
		try {
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.append("[ " + String.format("%4s", "-") + " ] ");
			writerSummary.append("[ ------------------------- ] ");
			writerSummary.append(filePath);
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.println("");
		System.out.println("[ " + String.format("%4s", "-") + " ] [ ------------------------- ] " + filePath);
	}

	private void appendQueryNumber() {
		count++;

		try {
			writerSummary.append("[ " + String.format("%4d", count) + " ] ");
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.print("[ " + String.format("%4d", count) + " ] ");
	}

	private void appendQuerySummary(String filePath) {
		try {
			writerSummary.append(filePath);
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.println(filePath);

		/* debug */
		if (databaseManager.getErrorBuffer().length() > 0) {
			try {
				Thread.sleep(100); /* Without it, the output order may be reversed. */
			} catch (InterruptedException e) {
				e.printStackTrace();
			}

			try {
				writerSummary.append(databaseManager.getErrorBuffer().toString());
				writerSummary.append(System.getProperty("line.separator"));
				writerSummary.flush();
			} catch (IOException e) {
				e.printStackTrace();
			}

			System.err.println(databaseManager.getErrorBuffer().toString());

			databaseManager.resetErrorBuffer();
		}
		/**/
	}

	public void appendResultSummary(int total, int success, int error) {
		subTotalCount += total;
		subTotalSuccessCount += success;
		subTotalErrorCount += error;

		try {
			writerSummary.append("[ T: " + String.format("%4d", total));
			writerSummary.append(", S: " + String.format("%4d", success));
			if (error > 0) {
				writerSummary.append(", E+ " + String.format("%4d", error) + " ] ");
			} else {
				writerSummary.append(", E: " + String.format("%4d", error) + " ] ");
			}
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.print("[ T: " + String.format("%4d", total));
		System.out.print(", S: " + String.format("%4d", success));
		if (error > 0) {
			System.out.print(", E+ " + String.format("%4d", error) + " ] ");
		} else {
			System.out.print(", E: " + String.format("%4d", error) + " ] ");
		}
	}

	public void appendSubTotalSummary() {
		try {
			writerSummary.append("         ");
			writerSummary.append("[ T: " + String.format("%4d", subTotalCount));
			writerSummary.append(", S: " + String.format("%4d", subTotalSuccessCount));
			if (subTotalErrorCount > 0) {
				writerSummary.append(", E+ " + String.format("%4d", subTotalErrorCount) + " ] ");
			} else {
				writerSummary.append(", E: " + String.format("%4d", subTotalErrorCount) + " ] ");
			}
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.print("         ");
		System.out.print("[ T: " + String.format("%4d", subTotalCount));
		System.out.print(", S: " + String.format("%4d", subTotalSuccessCount));
		if (subTotalErrorCount > 0) {
			System.out.print(", E+ " + String.format("%4d", subTotalErrorCount) + " ] ");
		} else {
			System.out.print(", E: " + String.format("%4d", subTotalErrorCount) + " ] ");
		}
		System.out.println("");

		totalCount += subTotalCount;
		totalSuccessCount += subTotalSuccessCount;
		totalErrorCount += subTotalErrorCount;

		subTotalCount = 0;
		subTotalSuccessCount = 0;
		subTotalErrorCount = 0;
	}

	public void appendTotalSummary() {
		try {
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.append("Summary: ");
			writerSummary.append("[ T: " + String.format("%4d", totalCount));
			writerSummary.append(", S: " + String.format("%4d", totalSuccessCount));
			if (totalErrorCount > 0) {
				writerSummary.append(", E+ " + String.format("%4d", totalErrorCount) + " ] ");
			} else {
				writerSummary.append(", E: " + String.format("%4d", totalErrorCount) + " ] ");
			}
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.flush();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.println("");
		System.out.print("Summary: ");
		System.out.print("[ T: " + String.format("%4d", totalCount));
		System.out.print(", S: " + String.format("%4d", totalSuccessCount));
		if (totalErrorCount > 0) {
			System.out.print(", E+ " + String.format("%4d", totalErrorCount) + " ] ");
		} else {
			System.out.print(", E: " + String.format("%4d", totalErrorCount) + " ] ");
		}
		System.out.println("");

		totalCount = 0;
		totalSuccessCount = 0;
		totalErrorCount = 0;
	}

	private void closeSummary() {
		try {
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.append("End SQL Analyzer.");
			writerSummary.append(System.getProperty("line.separator"));
			writerSummary.flush();
			writerSummary.close();
		} catch (IOException e) {
			System.err.println(e.getMessage());
		}

		System.out.println("");
		System.out.println("End SQL Analyzer.");
		System.out.println("");
	}

	public void start() {
		openSummary();

		File fileOrDir = new File(SQLMAP_PATH);

		traverseDirectory(fileOrDir);

		if (isBeforeFile == true) {
			appendSubTotalSummary();
		}

		appendTotalSummary();
		closeSummary();
	}

	private void traverseDirectory(File fileOrDir) {
		if (fileOrDir.isDirectory()) {
			File[] listFiles = fileOrDir.listFiles();

			for (int i = 0; i < listFiles.length; i++) {
				File file = listFiles[i];

				if (file.isFile() && file.getName().matches(".*.xml")) {
					isBeforeFile = true;

					if (file.length() == 0) {
						continue;
					}

					appendQueryNumber();
					parse(file);

					appendQuerySummary(rootPath.relativize(file.toURI()).toString());

					/*-
					 * debug
					 *
					try {
						appendQuerySummary(file.getCanonicalPath().toString());
					} catch (IOException e) {
						System.err.println(e.getMessage());
					}
					/**/

					continue;
				}

				if (file.isDirectory()) {
					count = 0;

					if (isBeforeFile == true) {
						appendSubTotalSummary();
						isBeforeFile = false;
					}

					appendDirectorySummary(rootPath.relativize(file.toURI()).toString());
					traverseDirectory(file);
				}
			}
		}
	}

	private void parse(File file) {
		String filePath = rootPath.relativize(file.toURI()).toString();
		String fileName = file.getName();

		SqlMapParser sqlMapParser = new SqlMapParser();
		sqlMapParser.analyze(this, databaseManager, filePath, fileName);
	}
}
