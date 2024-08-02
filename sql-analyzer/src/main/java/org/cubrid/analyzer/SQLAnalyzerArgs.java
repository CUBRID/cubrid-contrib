package org.cubrid.analyzer;

import com.beust.jcommander.Parameter;

public class SQLAnalyzerArgs {
	public static final String HELP = "--help";
	@Parameter(names = HELP, description = "Display usage information", help = true)
	private boolean help = false;

	public boolean getHelp() {
		return help;
	}

	public static final String SYNTAX_ONLY = "--syntax-only";
	@Parameter(names = SYNTAX_ONLY, description = "No database connection, only syntax check", required = false)
	private boolean syntaxOnly = false;

	public boolean getSyntaxOnly() {
		return syntaxOnly;
	}
}
