package org.cubrid.validator;

public class SQLValidator {

	static {
		try {
			String osName = System.getProperty("os.name").toLowerCase();

			/* -Djava.library.path=jni */
			if (osName.contains("win")) {
				System.loadLibrary("sqlvalidator");
			}
		} catch (UnsatisfiedLinkError e) {
			e.printStackTrace();
		}
	}

	// java -XshowSettings:properties -version
	public native String validateSQL(String query);

	public static void main(String[] args) {
		System.out.println(System.getProperty("user.dir"));
		SQLValidator jniExample = new SQLValidator();
		String result = jniExample.validateSQL("select * from game;");
		System.err.println(result);
	}
}
