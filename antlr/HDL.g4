grammar HDL;

// PARSER RULES

chip : KW_CHIP name=UPPER_ID genericParams? flag* constSection? inSection* outSection* partsSection? EOF ;

genericParams : LANGLE params+=UPPER_ID (COMMA? params+=UPPER_ID)* RANGLE ;

flag : AT name=LOWER_ID (LPAREN data=(UPPER_ID | LOWER_ID | INTEGER | PATH) RPAREN)? ;
partTag : AT tag=UPPER_ID ;

inSection    : KW_IN sigs+=signal (COMMA? sigs+=signal)* ;
outSection   : KW_OUT sigs+=signal (COMMA? sigs+=signal)* ;
constSection : KW_CONST constants+=constDef (COMMA? constants+=constDef)* ;
partsSection : KW_PARTS parts+=part (COMMA? parts+=part)* ;

scalar : UPPER_ID | INTEGER ;
number : UPPER_ID | INTEGER | LITERAL ;
constDef : name=UPPER_ID ASSIGN value=number ;

signal : name=LOWER_ID (LBRACK indices+=scalar RBRACK)* ;
slice  : signal (LBRACK lsb=scalar COLON msb=scalar RBRACK)? ;

lhsItem : slice | UNDERSCORE ;
lhs
    : lhsItem                                               # LhsSingle
    | LBRACE items+=lhsItem (COMMA? items+=lhsItem)* RBRACE # LhsConcat
    ;

repVal : slice | LITERAL ;
replication : count=scalar LBRACE val=repVal RBRACE ;

rhsItem : slice | number | replication ;
rhs
    : rhsItem                                               # RhsSingle
    | UNDERSCORE                                            # RhsDiscard
    | LBRACE items+=rhsItem (COMMA? items+=rhsItem)* RBRACE # RhsConcat
    ;

connection  : param=LOWER_ID ASSIGN val=rhs ;
partArgs    : conns+=connection (COMMA? conns+=connection)* ;
genericArgs : LANGLE args+=number (COMMA? args+=number)* RANGLE ;

part
    : type=UPPER_ID genericArgs? LPAREN partArgs? RPAREN partTag? # PartInst
    | lhs ASSIGN rhs                                              # PartAssign
    ;

// LEXER RULES (TOKENS)

// KEYWORDS
KW_CHIP  : 'CHIP' ;
KW_IN    : 'IN' ;
KW_OUT   : 'OUT' ;
KW_CONST : 'CONST' ;
KW_PARTS : 'PARTS' ;

// SYMBOLS
LPAREN     : '(' ;
RPAREN     : ')' ;
LBRACE     : '{' ;
RBRACE     : '}' ;
LBRACK     : '[' ;
RBRACK     : ']' ;
LANGLE     : '<' ;
RANGLE     : '>' ;
COMMA      : ',' ;
COLON      : ':' ;
ASSIGN     : '=' ;
AT         : '@' ;
UNDERSCORE : '_' ;

// VALUES AND IDENTIFIERS
INTEGER : '0' | [1-9][_0-9]* ;
LITERAL : INTEGER '\'' (DEN | BIN | HEX) ;

UPPER_ID : [A-Z][a-zA-Z0-9_]* ; // Chips, constants, generics
LOWER_ID : [a-z][a-zA-Z0-9_]* ; // Wires, pins, tags
PATH     : [a-zA-Z0-9_./\\]+ ;

// FRAGMENTS
fragment DEN : 'd' [_0-9]+ ;
fragment BIN : 'b' [_01]+ ;
fragment HEX : 'x' [_0-9a-fA-F]+ ;

// SKIPPED
WS            : [ ;\t\r\n]+ -> skip ;
COMMENT       : '//' ~[\r\n]* -> skip ;
BLOCK_COMMENT : '/*' .*? '*/' -> skip ;