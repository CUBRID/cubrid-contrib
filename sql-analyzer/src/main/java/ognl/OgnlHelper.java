package ognl;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import org.apache.ibatis.ognl.ASTAdd;
import org.apache.ibatis.ognl.ASTAnd;
import org.apache.ibatis.ognl.ASTAssign;
import org.apache.ibatis.ognl.ASTBitAnd;
import org.apache.ibatis.ognl.ASTBitNegate;
import org.apache.ibatis.ognl.ASTBitOr;
import org.apache.ibatis.ognl.ASTChain;
import org.apache.ibatis.ognl.ASTConst;
import org.apache.ibatis.ognl.ASTCtor;
import org.apache.ibatis.ognl.ASTDivide;
import org.apache.ibatis.ognl.ASTEq;
import org.apache.ibatis.ognl.ASTEval;
import org.apache.ibatis.ognl.ASTGreater;
import org.apache.ibatis.ognl.ASTGreaterEq;
import org.apache.ibatis.ognl.ASTIn;
import org.apache.ibatis.ognl.ASTInstanceof;
import org.apache.ibatis.ognl.ASTKeyValue;
import org.apache.ibatis.ognl.ASTLessEq;
import org.apache.ibatis.ognl.ASTList;
import org.apache.ibatis.ognl.ASTMap;
import org.apache.ibatis.ognl.ASTMethod;
import org.apache.ibatis.ognl.ASTMultiply;
import org.apache.ibatis.ognl.ASTNegate;
import org.apache.ibatis.ognl.ASTNot;
import org.apache.ibatis.ognl.ASTNotEq;
import org.apache.ibatis.ognl.ASTNotIn;
import org.apache.ibatis.ognl.ASTOr;
import org.apache.ibatis.ognl.ASTProject;
import org.apache.ibatis.ognl.ASTProperty;
import org.apache.ibatis.ognl.ASTRemainder;
import org.apache.ibatis.ognl.ASTRootVarRef;
import org.apache.ibatis.ognl.ASTSelect;
import org.apache.ibatis.ognl.ASTSelectFirst;
import org.apache.ibatis.ognl.ASTSelectLast;
import org.apache.ibatis.ognl.ASTSequence;
import org.apache.ibatis.ognl.ASTShiftLeft;
import org.apache.ibatis.ognl.ASTShiftRight;
import org.apache.ibatis.ognl.ASTStaticField;
import org.apache.ibatis.ognl.ASTStaticMethod;
import org.apache.ibatis.ognl.ASTSubtract;
import org.apache.ibatis.ognl.ASTTest;
import org.apache.ibatis.ognl.ASTThisVarRef;
import org.apache.ibatis.ognl.ASTUnsignedShiftRight;
import org.apache.ibatis.ognl.ASTVarRef;
import org.apache.ibatis.ognl.ASTXor;
import org.apache.ibatis.ognl.ExpressionNode;
import org.apache.ibatis.ognl.Node;
import org.apache.ibatis.ognl.Ognl;
import org.apache.ibatis.ognl.OgnlContext;
import org.apache.ibatis.ognl.OgnlException;

public final class OgnlHelper {
	private final String PSEUDO_VALUE = "1";

	private final String UNSUPPORTED = "unsupported";
	private final String GET_VALUE_BODY = "getValueBody";

	private Map<Class, String> methodMap = null;
	private Map<String, Object> expressionCache = null;

	private StringBuilder errorBuffer = null;

	public OgnlHelper() {
		methodMap = new ConcurrentHashMap<>();
		expressionCache = new ConcurrentHashMap<>();

		try {
			initNodeMethodMap();
		} catch (NoSuchMethodException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
		} catch (SecurityException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
		}

		errorBuffer = new StringBuilder();
	}

	private void initNodeMethodMap() throws NoSuchMethodException, SecurityException {
		/*-
		extends SimpleNode
		
		public String toString() {
		    StringBuilder result = new StringBuilder((parent == null) ? "" : "(");
		
		    if ((children != null) && (children.length > 0)) {
		        for (int i = 0; i < children.length; ++i) {
		            if (i > 0) {
		                result.append(" ").append(getExpressionOperator(i)).append(" ");
		            }
		            result.append(children[i].toString());
		        }
		    }
		    if (parent != null) {
		        result.append(")");
		    }
		    return result.toString();
		}
		*/
		methodMap.put(ExpressionNode.class, UNSUPPORTED);

		/*-
		extends ExpressionNode
		
		public String getExpressionOperator(int index) {
		    return (index == 1) ? "?" : ":";
		}
		*/
		methodMap.put(ASTTest.class, UNSUPPORTED);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		
		public String getExpressionOperator(int index) {
		    return "!";
		}
		*/
		methodMap.put(ASTNot.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		
		public String getExpressionOperator(int index) {
		    return "&&";
		}
		*/
		methodMap.put(ASTAnd.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		
		public String getExpressionOperator(int index) {
		    return "||";
		}
		*/
		methodMap.put(ASTOr.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		extends ComparisonExpression
		
		public String getExpressionOperator(int index) {
		    return "==";
		}
		*/
		methodMap.put(ASTEq.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		extends ComparisonExpression
		
		public String getExpressionOperator(int index) {
		    return "!=";
		}
		*/
		methodMap.put(ASTNotEq.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		extends ComparisonExpression
		
		public String getExpressionOperator(int index) {
		    return "<";
		}
		*/
		// ASTFunctionByClass.put(ASTLess.class, UNSUPPORTED); /* not visible */

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		extends ComparisonExpression
		
		public String getExpressionOperator(int index) {
		    return "<=";
		}
		*/
		methodMap.put(ASTLessEq.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		extends ComparisonExpression
		
		public String getExpressionOperator(int index) {
		    return ">";
		}
		*/
		methodMap.put(ASTGreater.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends BooleanExpression
		extends ComparisonExpression
		
		public String getExpressionOperator(int index) {
		    return ">=";
		}
		*/
		methodMap.put(ASTGreaterEq.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String toString() {
		    return "-" + children[0];
		}
		*/
		methodMap.put(ASTNegate.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression 
		
		public String getExpressionOperator(int index) {
		    return "+";
		}
		*/
		methodMap.put(ASTAdd.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "-";
		}
		*/
		methodMap.put(ASTSubtract.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "*";
		}
		*/
		methodMap.put(ASTMultiply.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "/";
		}
		*/
		methodMap.put(ASTDivide.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "%";
		}
		*/
		methodMap.put(ASTRemainder.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String toString() {
		    return "~" + children[0];
		}
		*/
		methodMap.put(ASTBitNegate.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "&";
		}
		*/
		methodMap.put(ASTBitAnd.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "|";
		}
		*/
		methodMap.put(ASTBitOr.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "^";
		}
		*/
		methodMap.put(ASTXor.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return "<<";
		}
		*/
		methodMap.put(ASTShiftLeft.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return ">>";
		}
		*/
		methodMap.put(ASTShiftRight.class, GET_VALUE_BODY);

		/*-
		extends ExpressionNode implements NodeType
		extends NumericExpression
		
		public String getExpressionOperator(int index) {
		    return ">>>";
		}
		*/
		methodMap.put(ASTUnsignedShiftRight.class, GET_VALUE_BODY);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return children[0] + " = " + children[1];
		}
		*/
		methodMap.put(ASTAssign.class, UNSUPPORTED);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return getKey() + " -> " + getValue();
		}
		*/
		methodMap.put(ASTKeyValue.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    return children[0] + " instanceof " + targetType;
		}
		*/
		methodMap.put(ASTInstanceof.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    return children[0] + " in " + children[1];
		}
		*/
		methodMap.put(ASTIn.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    return children[0] + " not in " + children[1];
		}
		*/
		methodMap.put(ASTNotIn.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    StringBuilder result = new StringBuilder("{ ");
		
		    for (int i = 0; i < jjtGetNumChildren(); ++i) {
		        if (i > 0) {
		            result.append(", ");
		        }
		        result.append(children[i].toString());
		    }
		    return result + " }";
		}
		*/
		methodMap.put(ASTList.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType, OrderedReturn
			
		public String toString() {
		    StringBuilder result = new StringBuilder();
		
		    if ((children != null) && (children.length > 0)) {
		        for (int i = 0; i < children.length; i++) {
		            if (i > 0) {
		                if (!(children[i] instanceof ASTProperty) || !((ASTProperty) children[i]).isIndexedAccess()) {
		                    result.append(".");
		                }
		            }
		            result.append(children[i].toString());
		        }
		    }
		    return result.toString();
		}
		*/
		methodMap.put(ASTChain.class, GET_VALUE_BODY);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    String result;
		
		    if (value == null) {
		        result = "null";
		    } else {
		        if (value instanceof String) {
		            result = '\"' + OgnlOps.getEscapeString(value.toString()) + '\"';
		        } else {
		            if (value instanceof Character) {
		                result = '\'' + OgnlOps.getEscapedChar((Character) value) + '\'';
		            } else {
		                result = value.toString();
		
		                if (value instanceof Long) {
		                    result = result + "L";
		                } else {
		                    if (value instanceof BigDecimal) {
		                        result = result + "B";
		                    } else {
		                        if (value instanceof BigInteger) {
		                            result = result + "H";
		                        } else {
		                            if (value instanceof Node) {
		                                result = ":[ " + result + " ]";
		                            }
		                        }
		                    }
		                }
		            }
		        }
		    }
		    return result;
		}
		*/
		methodMap.put(ASTConst.class, GET_VALUE_BODY);

		/*-
		extends SimpleNode
			
		public String toString() {
		    StringBuilder result = new StringBuilder("new " + className);
		
		    if (isArray) {
		        if (children[0] instanceof ASTConst) {
		            result.append("[").append(children[0]).append("]");
		        } else {
		            result.append("[] ").append(children[0]);
		        }
		    } else {
		        result.append("(");
		        if ((children != null) && (children.length > 0)) {
		            for (int i = 0; i < children.length; i++) {
		                if (i > 0) {
		                    result.append(", ");
		                }
		                result.append(children[i]);
		            }
		        }
		        result.append(")");
		    }
		    return result.toString();
		}
		*/
		methodMap.put(ASTCtor.class, UNSUPPORTED);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return "(" + children[0] + ")(" + children[1] + ")";
		}
		*/
		methodMap.put(ASTEval.class, UNSUPPORTED);

		/*-
		extends SimpleNode
			
		public String toString() {
		    StringBuilder result = new StringBuilder("#");
		
		    if (className != null) {
		        result.append("@").append(className).append("@");
		    }
		
		    result.append("{ ");
		    for (int i = 0; i < jjtGetNumChildren(); ++i) {
		        ASTKeyValue kv = (ASTKeyValue) children[i];
		
		        if (i > 0) {
		            result.append(", ");
		        }
		        result.append(kv.getKey()).append(" : ").append(kv.getValue());
		    }
		    return result + " }";
		}
		*/
		methodMap.put(ASTMap.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements OrderedReturn, NodeType
			
		public String toString() {
		    StringBuilder result = new StringBuilder(methodName);
		
		    result.append("(");
		    if ((children != null) && (children.length > 0)) {
		        for (int i = 0; i < children.length; i++) {
		            if (i > 0) {
		                result.append(", ");
		            }
		            result.append(children[i]);
		        }
		    }
		
		    result.append(")");
		    return result.toString();
		}
		*/
		methodMap.put(ASTMethod.class, GET_VALUE_BODY);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return "{ " + children[0] + " }";
		}
		*/
		methodMap.put(ASTProject.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    String result;
		
		    if (isIndexedAccess()) {
		        result = "[" + children[0] + "]";
		    } else {
		        result = ((ASTConst) children[0]).getValue().toString();
		    }
		    return result;
		}
		*/
		methodMap.put(ASTProperty.class, GET_VALUE_BODY);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return "{? " + children[0] + " }";
		}
		*/
		methodMap.put(ASTSelect.class, UNSUPPORTED);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return "{^ " + children[0] + " }";
		}
		*/
		methodMap.put(ASTSelectFirst.class, UNSUPPORTED);

		/*-
		extends SimpleNode
			
		public String toString() {
		    return "{$ " + children[0] + " }";
		}
		*/
		methodMap.put(ASTSelectLast.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType, OrderedReturn
			
		public String toString() {
		    StringBuilder result = new StringBuilder();
		
		    for (int i = 0; i < children.length; ++i) {
		        if (i > 0) {
		            result.append(", ");
		        }
		        result.append(children[i]);
		    }
		    return result.toString();
		}
		*/
		methodMap.put(ASTSequence.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
			
		public String toString() {
		    return "@" + className + "@" + fieldName;
		}
		*/
		methodMap.put(ASTStaticField.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType
		
		public String toString() {
		    StringBuilder result = new StringBuilder("@" + className + "@" + methodName);
		
		    result.append("(");
		    if ((children != null) && (children.length > 0)) {
		        for (int i = 0; i < children.length; i++) {
		            if (i > 0) {
		                result.append(", ");
		            }
		            result.append(children[i]);
		        }
		    }
		    result.append(")");
		    return result.toString();
		}
		*/
		methodMap.put(ASTStaticMethod.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType, OrderedReturn
		
		public String toString() {
		    return "#" + name;
		}
		*/
		methodMap.put(ASTVarRef.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType, OrderedReturn
		extends ASTVarRef
		
		public String toString() {
		    return "#root";
		}
		*/
		methodMap.put(ASTRootVarRef.class, UNSUPPORTED);

		/*-
		extends SimpleNode implements NodeType, OrderedReturn
		extends ASTVarRef
		
		public String toString() {
		    return "#this";
		}
		*/
		methodMap.put(ASTThisVarRef.class, UNSUPPORTED);
	}

	private Object parseExpression(String expression) {
		Object node = expressionCache.get(expression);

		if (node == null) {
			try {
				node = Ognl.parseExpression(expression);
			} catch (OgnlException e) {
				errorBuffer.append(e.toString());
				errorBuffer.append(System.getProperty("line.separator"));
				return null;
			}

			expressionCache.put(expression, node);
		}

		return node;
	}

	public Boolean getValue(String expression, Object root) {
		Object expr = parseExpression(expression);
		if (expr == null) {
			return false;
		}

		Node node = Node.class.cast(expr);
		Map context = Ognl.createDefaultContext(root);

		Boolean isSuccess = this.processExpression(node, context, root);
		if (!isSuccess) {
			return false;
		}

		Object o = null;
		try {
			o = node.getValue(OgnlContext.class.cast(context), root);
		} catch (OgnlException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
			return false;
		} catch (NumberFormatException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
			return false;
		}

		return Boolean.class.cast(o);
	}

	private Boolean processExpression(Node node, Map context, Object root) {
		String methodName = methodMap.get(node.getClass());
		if (UNSUPPORTED.equals(methodName)) {
			try {
				throw new OgnlException("Error unsupported class '" + node.getClass().getName() + "'");
			} catch (OgnlException e) {
				errorBuffer.append(e.toString());
				errorBuffer.append(System.getProperty("line.separator"));
			}

			return false;
		}

		Method m = null;
		try {
			m = OgnlHelper.class.getMethod(methodName, Node.class, Map.class, Object.class);
		} catch (NoSuchMethodException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
		} catch (SecurityException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
		}

		if (m == null) {
			return false;
		}

		try {
			m.invoke(this, node, context, root);
		} catch (IllegalAccessException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
			return false;
		} catch (IllegalArgumentException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
			return false;
		} catch (InvocationTargetException e) {
			errorBuffer.append(e.toString());
			errorBuffer.append(System.getProperty("line.separator"));
			return false;
		}

		return true;
	}

	public void getValueBody(Node node, Map context, Object root) {
		if (ASTProperty.class.equals(node.getClass())) {
			this.processProperty(node, context, root);
		} else {
			for (int i = 0; i < node.jjtGetNumChildren(); i++) {
				this.getValueBody(node.jjtGetChild(i), context, root);
			}
		}
	}

	public void processProperty(Node node, Map context, Object root) {
		Node parent = node.jjtGetParent();
		Node child = parent.jjtGetChild(1);

		Map<String, Object> propertyMap = Map.class.cast(root);
		String propertyName = node.toString();
		Object propertyValue = null;
		Node target = null;

		if (!(propertyMap.containsKey(propertyName))) {
			if (ASTMethod.class.equals(child.getClass())) {
				if (ASTChain.class.equals(parent.getClass())) {
					if (child.jjtGetNumChildren() == 1) {
						target = child.jjtGetChild(0);
					}
				}
			} else {
				target = child;
			}

			if (target == null) {
				return;
			}

			try {
				propertyValue = target.getValue(OgnlContext.class.cast(context), root);
			} catch (OgnlException e) {
				errorBuffer.append(e.toString());
				errorBuffer.append(System.getProperty("line.separator"));
				return;
			}

			if (ASTNotEq.class.equals(parent.getClass()) && propertyValue == null) {
				propertyValue = PSEUDO_VALUE;
			}

			propertyMap.put(propertyName, propertyValue);
		}
	}

	public StringBuilder getErrorBuffer() {
		return errorBuffer;
	}

	public void resetErrorBuffer() {
		errorBuffer.setLength(0);
	}
}