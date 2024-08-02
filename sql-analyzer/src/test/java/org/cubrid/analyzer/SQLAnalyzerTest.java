package org.cubrid.analyzer;

import org.junit.Test;

public class SQLAnalyzerTest {

	@Test
	public void validatorTest() {
		SQLAnalyzer sqlAnalyzer = new SQLAnalyzer(true);
		sqlAnalyzer.start();
	}

	@Test
	public void analyzerTest() {
		SQLAnalyzer sqlAnalyzer = new SQLAnalyzer(false);
		sqlAnalyzer.start();
	}

}
