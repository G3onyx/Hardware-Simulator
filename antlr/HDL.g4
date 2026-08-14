grammar HDL;

// PARSER RULES

chip : KW_CHIP name=ID genericParams? flag* constSection? declaration* partsSection? EOF ;

genericParams : LANGLE params+=ID (COMMA? params+=ID)* RANGLE ;

flag : AT name=ID (LPAREN data=(ID | INTEGER | PATH) RPAREN)? ;

declaration
    : inSection
    | outSection
    | wiresSection
    ;

inSection    : KW_IN sigs+=signal (COMMA? sigs+=signal)* ;
outSection   : KW_OUT sigs+=signal (COMMA? sigs+=signal)* ;
wiresSection  : KW_WIRES sigs+=signal (COMMA? sigs+=signal)* ;
constSection : KW_CONST constants+=constDef (COMMA? constants+=constDef)* ;
partsSection : KW_PARTS parts+=part (COMMA? parts+=part)* ;

scalar : ID | INTEGER ;
number : ID | INTEGER | LITERAL ;
constDef : ID ASSIGN number ;

signal : name=ID (LBRACK indices+=scalar RBRACK)* ;
slice  : signal (LBRACK lsb=scalar COLON msb=scalar RBRACK)? ;

lhsItem : slice | UNDERSCORE ;
lhs
    : lhsItem                                               # LhsSingle
    | LBRACE items+=lhsItem (COMMA? items+=lhsItem)* RBRACE # LhsConcat
    ;

repVal : slice | LITERAL ;
replication : count=scalar LBRACE val=repVal RBRACE ;

rhsItem : slice | LITERAL | replication ;
rhs
    : rhsItem                                               # RhsSingle
    | UNDERSCORE                                            # RhsDiscard
    | LBRACE items+=rhsItem (COMMA? items+=rhsItem)* RBRACE # RhsConcat
    ;

connection  : param=ID ASSIGN val=rhs ;
partArgs    : conns+=connection (COMMA? conns+=connection)* ;
genericArgs : LANGLE args+=number (COMMA? args+=number)* RANGLE ;

partTag : AT tag=ID ;
part
    : type=ID genericArgs? LPAREN partArgs? RPAREN partTag? # PartInst
    | lhs ASSIGN rhs                                        # PartAssign
    ;


// LEXER RULES (TOKENS)

// KEYWORDS
KW_CHIP  : 'CHIP' ;
KW_IN    : 'IN' ;
KW_OUT   : 'OUT' ;
KW_WIRES  : 'WIRES' ;
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

ID : [a-zA-Z_][a-zA-Z0-9_]* ;
PATH : [a-zA-Z0-9_./\\]+ ;

// FRAGMENTS
fragment DEN : 'd' [_0-9]+ ;
fragment BIN : 'b' [_01]+ ;
fragment HEX : 'x' [_0-9a-fA-F]+ ;

// SKIPPED
WS            : [ ;\t\r\n]+ -> skip ;
COMMENT       : '//' ~[\r\n]* -> skip ;
BLOCK_COMMENT : '/*' .*? '*/' -> skip ;