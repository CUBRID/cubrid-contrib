package org.cubrid.parser;

public class SqlMapTag {
	private String name = null;
	private String id = null;
	private String property = null;
	private String prepend = null;
	private String compareValue = null;
	private String open = null;
	private String close = null;
	private String conjunction = null;
	private String test = null;
	private StringBuffer contents = null;

	public SqlMapTag() {
		contents = new StringBuffer();
		contents.setLength(0);
	}

	public String getName() {
		return name;
	}

	public void setName(String name) {
		this.name = name;
	}

	public String getId() {
		return id;
	}

	public void setId(String id) {
		this.id = id;
	}

	public String getProperty() {
		return property;
	}

	public void setProperty(String property) {
		this.property = property;
	}

	public String getPrepend() {
		return prepend;
	}

	public void setPrepend(String prepend) {
		this.prepend = prepend;
	}

	public String getCompareValue() {
		return compareValue;
	}

	public void setCompareValue(String compareValue) {
		this.compareValue = compareValue;
	}

	public String getOpen() {
		return open;
	}

	public void setOpen(String open) {
		this.open = open;
	}

	public String getClose() {
		return close;
	}

	public void setClose(String close) {
		this.close = close;
	}

	public String getConjunction() {
		return conjunction;
	}

	public void setConjunction(String conjunction) {
		this.conjunction = conjunction;
	}

	public String getTest() {
		return test;
	}

	public void setTest(String test) {
		this.test = test;
	}
	
	public String getContents() {
		return new String(contents);
	}

	public int getLength() {
		return contents.length();
	}

	public void setContents(String contents) {
		this.contents = new StringBuffer(contents);
	}

	public SqlMapTag addContents(String contents) {
		this.contents.append(contents);
		return this;
	}

	public void clear(String contents) {
		this.contents.setLength(0);
	}

	public void print() {
		System.out.println("------------------------------------------------------------");
		System.out.println("Tag Name          : " + name);
		System.out.println("Tag Id            : " + id);
		System.out.println("Tag Property      : " + property);
		System.out.println("Tag Prepend       : " + prepend);
		System.out.println("Tag CompareValue  : " + compareValue);
		System.out.println("Tag Open          : " + open);
		System.out.println("Tag Close         : " + close);
		System.out.println("Tag Conjunction   : " + conjunction);
		System.out.println("Tag Test          : " + test);
		System.out.println("Tag Contents Size : " + contents.length());
		System.out.println("------------------------------------------------------------");
	}
}
