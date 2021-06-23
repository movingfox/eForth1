#include "eforth_core.h"
#include "eforth_asm.h"
//
// Forth Macro Assembler
//
#define fIMMED  0x80           		// immediate flag
#define fCOMPO  0x40                // compile only flag
//
// variables to keep branching addresses
//
XA BRAN, QBRAN, DONXT;
XA DOTQ, STRQ, ABORTQ;
XA TOR, NOP = 0xffff;				// NOP set to ffff to prevent access before initialized
//
// return stack for branching ops
//
U8 *aByte;                          // heap
U8 aR;                              // return stack index
XA aPC, aThread;                    // program counter, pointer to previous word
//
// memory access and stack op macros
//
#define BSET(d, c)  (*(aByte+(d))=(U8)(c))
#define BGET(d)     ((U8)*(aByte+(d)))
#define SET(d, v)   do { U16 a=(d); U16 x=(v); BSET(a, (x)&0xff); BSET((a)+1, (x)>>8); } while (0)
#define GET(d)      ({ U16 a=(d); (U16)BGET(a) + ((U16)BGET((a)+1)<<8); })
#define STORE(v)    do { SET(aPC, (v)); aPC+=CELLSZ; } while(0)
#define R_GET(r)    ((U16)GET(FORTH_ROM_SZ - (r)*CELLSZ))
#define R_SET(r, v) SET(FORTH_ROM_SZ - (r)*CELLSZ, v)
#define RPUSH(a)    R_SET(++aR, a)
#define RPOP()      R_GET(aR ? aR-- : aR)
#define VL(a, i)    (((U16)(a)+CELLSZ*(i))&0xff)
#define VH(a, i)    (((U16)(a)+CELLSZ*(i))>>8)

void _dump(int b, int u) {
    // dump memory between previous word and this
    DEBUG("%s", "\n    :");
    for (int i=b; i<u; i+=sizeof(XA)) {
        if ((i+1)<u) DEBUG(" %04x", GET(i));
        else         DEBUG(" %02x", BGET(i));
    }
    DEBUG("%c", '\n');
}
void _rdump()
{
	DEBUG("%cR[", ' ');
	for (int i=1; i<=aR; i++) {
        DEBUG(" %04x", R_GET(i));
	}
	DEBUG("%c]", ' ');
}
int _strlen(FCHAR *seq) {
    PGM_P p = reinterpret_cast<PGM_P>(seq);
    int i=0;
    for (; pgm_read_byte(p); i++, p++);
    return i;
}
void _header(int lex, FCHAR *seq) {
    if (aThread) {
        if (aPC >= FORTH_ROM_SZ) DEBUG("ROM %s", "max!");
        _dump(aThread-sizeof(XA), aPC);         // dump data from previous word to current word
    }
    STORE(aThread);                           // point to previous word
    aThread = aPC;                            // keep pointer to this word

    BSET(aPC++, lex);                         // length of word (with optional fIMMED or fCOMPO flags)
    int len = lex & 0x1f;                     // Forth allows word max length 31
    PGM_P p = reinterpret_cast<PGM_P>(seq);
    for (int i=0; i < len; i++) {             // memcpy word string
        BSET(aPC++, pgm_read_byte(p++));
    }
    DEBUG("%04x: ", aPC);
    DEBUG("%s", seq);
}
int _code(FCHAR *seg, int len, ...) {
    _header(_strlen(seg), seg);
    int addr = aPC;                           // keep address of current word
    va_list argList;
    va_start(argList, len);
    for (; len; len--) {                      // copy bytecodes
        U8 b = (U8)va_arg(argList, int);
        BSET(aPC++, b);
        DEBUG(" %02x", b);
    }
    va_end(argList);
    return addr;
}
#define CELLCPY(n) {                            \
    va_list argList;                            \
	va_start(argList, n);						\
	for (; n; n--) {							\
		XA j = (XA)va_arg(argList, int);        \
		if (j==NOP) continue;                   \
		STORE(j);								\
		DEBUG(" %04x", j);						\
	}											\
	va_end(argList);							\
	_rdump();                                   \
}
int _colon(FCHAR *seg, int len, ...) {
    _header(_strlen(seg), seg);
    DEBUG(" %s", ":06");
    int addr = aPC;
    BSET(aPC++, opENTER);
    CELLCPY(len);
    return addr;
}
int _immed(FCHAR *seg, int len, ...) {
    _header(fIMMED | _strlen(seg), seg);
    DEBUG(" %s", "i06");
    int addr = aPC;
    BSET(aPC++, opENTER);
    CELLCPY(len);
    return addr;
}
int _label(int len, ...) {
    SHOWOP("LABEL");
    int addr = aPC;
    // label has no opcode here
    CELLCPY(len);
    return addr;
}
void _begin(int len, ...) {
    SHOWOP("BEGIN");               // BEGIN-(once)-WHILE-(loop)-UNTIL/REPEAT
    RPUSH(aPC);                    // keep current address for looping
    CELLCPY(len);
}
void _while(int len, ...) {        
    SHOWOP("WHILE");
    STORE(QBRAN);
    STORE(0);                      // branching address
    int k = RPOP();
    RPUSH(aPC - CELLSZ);
    RPUSH(k);
    CELLCPY(len);
}
void _repeat(int len, ...) {
    SHOWOP("REPEAT");
    STORE(BRAN);
    STORE(RPOP());
    SET(RPOP(), aPC);
    CELLCPY(len);
}
void _until(int len, ...) {
    SHOWOP("UNTIL");
    STORE(QBRAN);                   // conditional branch
    STORE(RPOP());                  // loop begin address
    CELLCPY(len);
}
void _again(int len, ...) {         // BEGIN-AGAIN
    SHOWOP("AGAIN");
    STORE(BRAN);                    // unconditional branch
    STORE(RPOP());                  // store return address
    CELLCPY(len);
}
void _for(int len, ...) {
    SHOWOP("FOR");                 // FOR-(first)-AFT-(2nd,...)-THEN-(every)-NEXT
    STORE(TOR);                    // put loop counter on return stack
    RPUSH(aPC);                    // keep 1st loop repeat address A0
    CELLCPY(len);
}
void _aft(int len, ...) {          // code between FOR-AFT run only once
    SHOWOP("AFT");
    STORE(BRAN);                   // unconditional branch
    STORE(0);                      // forward jump address (A1)NOP,
    RPOP();                        // pop-off A0 (FOR-AFT once only)
    RPUSH(aPC);                    // keep repeat address on return stack
    RPUSH(aPC - CELLSZ);           // keep A1 address on return stack for AFT-THEN
    CELLCPY(len);
}
void _nxt(int len, ...) {          // _next() is multi-defined in vm
    SHOWOP("NEXT");
    STORE(DONXT);                  // check loop counter (on return stack)
    STORE(RPOP());                 // add A0 (FOR-NEXT) or 
    CELLCPY(len);                  // A1 to repeat loop (conditional branch by DONXT)
}
void _if(int len, ...) {           // IF-THEN, IF-ELSE-THEN
    SHOWOP("IF");
	STORE(QBRAN);                  // conditional branch
    RPUSH(aPC);                    // keep A0 address on return stack for ELSE or THEN
    STORE(0);                      // reserve branching address (A0)
    CELLCPY(len);
}
void _else(int len, ...) {
    SHOWOP("ELSE");
    STORE(BRAN);                   // unconditional branch
    STORE(0);                      // reserve branching address (A1)
    SET(RPOP(), aPC);              // backfill A0 branching address
    RPUSH(aPC - CELLSZ);           // keep A1 address on return stack for THEN
    CELLCPY(len);
}
void _then(int len, ...) {
    SHOWOP("THEN");
    SET(RPOP(), aPC);              // backfill branching address (A0) or (A1)
    CELLCPY(len);
}
#define STRCPY(op, seq) {                           \
	STORE(op);                                      \
	int len = _strlen(seq);							\
    PGM_P p = reinterpret_cast<PGM_P>(seq);         \
	BSET(aPC++, len);                               \
	for (int i=0; i < len; i++) {					\
		BSET(aPC++, pgm_read_byte(p++));            \
	}												\
}
void _dotq(FCHAR *seq) {
    SHOWOP("DOTQ");
    DEBUG("%s", seq);
    STRCPY(DOTQ, seq);
}
void _strq(FCHAR *seq) {
    SHOWOP("STRQ");
    DEBUG("%s", seq);
    STRCPY(STRQ, seq);
}
void _abortq(FCHAR *seq) {
    SHOWOP("ABORTQ");
    DEBUG("%s", seq);
    STRCPY(ABORTQ, seq);
}
//
// assembler macros (calculate number of parameters by compiler)
//
#define _CODE(seg, ...)      _code(F(seg), _NARG(__VA_ARGS__), __VA_ARGS__)
#define _COLON(seg, ...)     _colon(F(seg), _NARG(__VA_ARGS__), __VA_ARGS__)
#define _IMMED(seg, ...)     _immed(F(seg), _NARG(__VA_ARGS__), __VA_ARGS__)
#define _LABEL(...)          _label(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _BEGIN(...)          _begin(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _AGAIN(...)          _again(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _UNTIL(...)          _until(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _WHILE(...)          _while(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _REPEAT(...)         _repeat(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _IF(...)             _if(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _ELSE(...)           _else(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _THEN(...)           _then(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _FOR(...)            _for(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _NEXT(...)           _nxt(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _AFT(...)            _aft(_NARG(__VA_ARGS__), __VA_ARGS__)
#define _DOTQ(seq)           _dotq(F(seq))
#define _STRQ(seq)           _strq(F(seq))
#define _ABORTQ(seq)         _abortq(F(seq))

int ef_assemble(U8 *cdata)
{
	aByte = cdata;
	aR    = aThread = 0;
	//
	// Kernel constants
	//
	aPC = FORTH_BOOT_ADDR;
    XA BOOT  = _LABEL(opENTER, 0);      // reserved for boot vectors

    XA ta    = FORTH_TVAR_ADDR;
	XA vHLD  = _CODE("HLD",     opDOCON, VL(ta,0), VH(ta,0));   // char pointer to output buffer
	XA vSPAN = _CODE("SPAN",    opDOCON, VL(ta,1), VH(ta,1));   // number of character accepted
	XA vIN   = _CODE(">IN",     opDOCON, VL(ta,2), VH(ta,2));   // interpreter pointer to next char
	XA vNTIB = _CODE("#TIB",    opDOCON, VL(ta,3), VH(ta,3));   // number of character received in TIB
    
	XA ua    = FORTH_UVAR_ADDR;
	XA vTTIB = _CODE("'TIB",    opDOCON, VL(ua,0), VH(ua,0));   // console input buffer pointer
	XA vBASE = _CODE("BASE",    opDOCON, VL(ua,1), VH(ua,1));   // current radix for numeric ops
	XA vCP   = _CODE("CP",      opDOCON, VL(ua,2), VH(ua,2));   // =HERE, top of dictionary
	XA vCNTX = _CODE("CONTEXT", opDOCON, VL(ua,3), VH(ua,3));   // name field of last word
	XA vLAST = _CODE("LAST",    opDOCON, VL(ua,4), VH(ua,4));   // =CONTEXT
	XA vTEVL = _CODE("'EVAL",   opDOCON, VL(ua,5), VH(ua,5));   // eval mode (interpreter or compiler)
	XA vTABRT= _CODE("'ABORT",  opDOCON, VL(ua,6), VH(ua,6));   // exception rescue handler (QUIT)
	XA vTEMP = _CODE("tmp",     opDOCON, VL(ua,7), VH(ua,7));   // tmp storage (alternative to return stack)
	//
	// common constants and variable spec
	//
	XA BLANK = _CODE("BL",      opDOCON, 0x20,      0);
	XA CELL  = _CODE("CELL",    opDOCON, CELLSZ,    0);
	//
	// Kernel dictionary (primitive words)
	//
       NOP   = _CODE("NOP",     opNOP    );
	XA BYE   = _CODE("BYE",     opBYE    );
	XA QRX   = _CODE("?RX",     opQRX    );
	XA TXSTO = _CODE("TX!",     opTXSTO  );
	XA DOCON = _CODE("DOCON",   opDOCON  );
	XA DOLIT = _CODE("DOLIT",   opDOLIT  );
	XA ENTER = _CODE("ENTER",   opENTER  );    // aka DOLIST by Dr. Ting
	XA EXIT  = _CODE("EXIT",    opEXIT   );
	XA EXECU = _CODE("EXECUTE", opEXECU  );
	   DONXT = _CODE("DONEXT",  opDONEXT );
	   QBRAN = _CODE("?BRANCH", opQBRAN  );
	   BRAN  = _CODE("BRANCH",  opBRAN   );
	XA STORE = _CODE("!",       opSTORE  );
	XA AT    = _CODE("@",       opAT     );
	XA CSTOR = _CODE("C!",      opCSTOR  );
	XA CAT   = _CODE("C@",      opCAT    );
    XA ONEP  = _CODE("1+",      opONEP   );
    XA ONEM  = _CODE("1-",      opONEM   );
	XA RFROM = _CODE("R>",      opRFROM  );
	XA RAT   = _CODE("R@",      opRAT    );
	   TOR   = _CODE(">R",      opTOR    );
    XA DEPTH = _CODE("DEPTH",   opDEPTH  );
	XA DROP  = _CODE("DROP",    opDROP   );
	XA DUP   = _CODE("DUP",     opDUP    );
	XA SWAP  = _CODE("SWAP",    opSWAP   );
	XA OVER  = _CODE("OVER",    opOVER   );
	XA ZLESS = _CODE("0<",      opZLESS  );
	XA AND   = _CODE("AND",     opAND    );
	XA OR    = _CODE("OR",      opOR     );
	XA XOR   = _CODE("XOR",     opXOR    );
	XA UPLUS = _CODE("UM+",     opUPLUS  );
	XA QDUP  = _CODE("?DUP",    opQDUP   );
	XA ROT   = _CODE("ROT",     opROT    );
    XA LSHFT = _CODE("<<",      opLSHIFT );
    XA RSHFT = _CODE(">>",      opRSHIFT );
	XA PLUS  = _CODE("+",       opPLUS   );
	XA INVER = _CODE("INVERT",  opINVERT );
	XA NEGAT = _CODE("NEGATE",  opNEGAT  );
    XA GREAT = _CODE(">",       opGREAT  );
	XA SUB   = _CODE("-",       opSUB    );
	XA ABS   = _CODE("ABS",     opABS    );
	XA EQUAL = _CODE("=",       opEQUAL  );
	XA ULESS = _CODE("U<",      opULESS  );
	XA LESS  = _CODE("<",       opLESS   );
	XA UMMOD = _CODE("UM/MOD",  opUMMOD  );
	XA MOD   = _CODE("MOD",     opMOD    );
    XA UMSTA = _CODE("UM*",     opUMSTAR );
	XA SLASH = _CODE("/",       opSLASH  );
	XA STAR  = _CODE("*",       opSTAR   );
	XA MSTAR = _CODE("M*",      opMSTAR  );
	XA PICK  = _CODE("PICK",    opPICK   );
	XA PSTOR = _CODE("+!",      opPSTOR  );
	XA COUNT = _CODE("COUNT",   opCOUNT  );
	XA DOVAR = _CODE("DOVAR",   opDOVAR  );
	XA MAX   = _CODE("MAX",     opMAX    );
	XA MIN   = _CODE("MIN",     opMIN    );
	//
	// Common Colon Words (in word streams)
	//
	XA HERE  = _COLON("HERE",  vCP, AT, EXIT);                          // top of dictionary
	XA PAD   = _COLON("PAD",   HERE, DOLIT, FORTH_PAD_SZ, PLUS, EXIT);  // use HERE for output buffer
	XA CELLP = _COLON("CELL+", CELL,  PLUS,  EXIT);
	XA CELLM = _COLON("CELL-", CELL,  SUB,   EXIT);
	XA CELLS = _COLON("CELLS", CELL,  STAR,  EXIT);
    XA DDUP  = _COLON("2DUP",  OVER, OVER, EXIT);
    XA DDROP = _COLON("2DROP", DROP, DROP, EXIT);
	XA WITHI = _COLON("WITHIN",  OVER, SUB, TOR, SUB, RFROM, ULESS, EXIT);
	XA CMOVE = _COLON("CMOVE", NOP); {
		_FOR(NOP);
		_AFT(OVER, CAT, OVER, CSTOR, TOR, ONEP, RFROM, ONEP);
		_THEN(NOP);
		_NEXT(DDROP, EXIT);
	}
	XA MOVE  = _COLON("MOVE", CELL, SLASH); {
		_FOR(NOP);
		_AFT(OVER, AT, OVER, STORE, TOR, CELLP, RFROM, CELLP);
		_THEN(NOP);
		_NEXT(DDROP, EXIT);
	}
	XA FILL = _COLON("FILL", SWAP); {
		_FOR(SWAP);
		_AFT(DDUP, CSTOR, ONEP);
		_THEN(NOP);
		_NEXT(DDROP, EXIT);
	}
	//
	// Number Conversions and formatting
	//
	XA HEX_  = _COLON("HEX",     DOLIT, 16, vBASE, STORE, EXIT);
	XA DECIM = _COLON("DECIMAL", DOLIT, 10, vBASE, STORE, EXIT);
	XA DIGIT = _COLON("DIGIT",   DOLIT, 9, OVER, LESS, DOLIT, 7, AND, PLUS, DOLIT, 0x30, PLUS, EXIT);
	XA EXTRC = _COLON("EXTRACT", DOLIT, 0, SWAP, UMMOD, SWAP, DIGIT, EXIT);
	XA BDIGS = _COLON("<#",      PAD, vHLD, STORE, EXIT);
	XA HOLD  = _COLON("HOLD",    vHLD, AT, ONEM, DUP, vHLD, STORE, CSTOR, EXIT);
	XA DIG   = _COLON("#",       vBASE, AT, EXTRC, HOLD, EXIT);
	XA DIGS  = _COLON("#S", NOP); {
		_BEGIN(DIG, DUP);
		_WHILE(NOP);
		_REPEAT(EXIT);
	}
	XA SIGN  = _COLON("SIGN",   ZLESS); {
		_IF(DOLIT, 0x2d, HOLD);
		_THEN(EXIT);
	}
	XA EDIGS = _COLON("#>",     DROP, vHLD, AT, PAD, OVER, SUB, EXIT);
	XA STR   = _COLON("str",    DUP, TOR, ABS, BDIGS, DIGS, RFROM, SIGN, EDIGS, EXIT);
    XA UPPER = _COLON("wupper", DOLIT, 0x5f5f, AND, EXIT);
	XA TOUPP = _COLON(">upper", DUP, DOLIT, 0x61, DOLIT, 0x7b, WITHI); { // [a-z] only?
		_IF(DOLIT, 0x5f, AND);
		_THEN(EXIT);
	}
	XA DIGTQ = _COLON("DIGIT?", TOR, TOUPP, DOLIT, 0x30, SUB, DOLIT, 9, OVER, LESS); {
		_IF(DOLIT, 7, SUB, DUP, DOLIT, 10, LESS, OR);           // handle hex number
		_THEN(DUP, RFROM, ULESS, EXIT);                         // handle base > 10
	}
	XA NUMBQ = _COLON("NUMBER?", vBASE, AT, TOR, DOLIT, 0, OVER, COUNT,
                      OVER, CAT, DOLIT, 0x24, EQUAL); {         // leading with $ (i.e. 0x24)
		_IF(HEX_, SWAP, ONEP, SWAP, ONEM);
		_THEN(OVER, CAT, DOLIT, 0x2d, EQUAL,                    // handle negative sign (i.e. 0x2d)
              TOR, SWAP, RAT, SUB, SWAP, RAT, PLUS, QDUP);
		_IF(ONEM); {
            // a FOR..WHILE..NEXT..ELSE..THEN construct =~ for {..break..}
            _FOR(DUP, TOR, CAT, vBASE, AT, DIGTQ);                    
			_WHILE(SWAP, vBASE, AT, STAR, PLUS, RFROM, ONEP);   // if digit, xBASE, else break to ELSE
            _NEXT(DROP, RAT); {                                 // whether negative number
                _IF(NEGAT);
			    _THEN(SWAP);
            }
  			_ELSE(RFROM, RFROM, DDROP, DDROP, DOLIT, 0);
			_THEN(DUP);
         }
  		 _THEN(RFROM, DDROP, RFROM, vBASE, STORE, EXIT);
	}
	//
	// Console I/O
	//
	XA TIB   = _COLON("TIB",   vTTIB, AT, EXIT);
	XA QKEY  = _COLON("?KEY",  QRX, EXIT);
	XA KEY   = _COLON("KEY",   NOP); {
		_BEGIN(QKEY);
		_UNTIL(EXIT);
	}
	XA EMIT  = _COLON("EMIT",  TXSTO, EXIT);
	XA HATH  = _COLON("^H", TOR, OVER, RFROM, SWAP, OVER, XOR); {
		_IF(DOLIT, 8, EMIT, ONEM, BLANK, EMIT, DOLIT, 8, EMIT);
		_THEN(EXIT);
	}
	XA SPACE = _COLON("SPACE", BLANK, EMIT, EXIT);
	XA CHARS = _COLON("CHARS", SWAP, DOLIT, 0, MAX); {
		_FOR(NOP);
		_AFT(DUP, EMIT);
		_THEN(NOP);
		_NEXT(DROP, EXIT);
	}
	XA TCHAR = _COLON(">CHAR", DOLIT, 0x7f, AND, DUP, DOLIT, 0x7f, BLANK, WITHI); {
		_IF(DROP, DOLIT, 0x5f);    // out-of-range put '_' instead
		_THEN(EXIT);
	}
	XA SPACS = _COLON("SPACES", BLANK, CHARS, EXIT);
	XA TYPE  = _COLON("TYPE", NOP); {
		_FOR(NOP);
		_AFT(COUNT, TCHAR, EMIT);
		_THEN(NOP);
		_NEXT(DROP, EXIT);
	}
	XA CR    = _COLON("CR",   DOLIT, 10, DOLIT, 13, EMIT, EMIT, EXIT);
	XA DOSTR = _COLON("do$",  RFROM, RAT, RFROM, COUNT, PLUS, TOR, SWAP, TOR, EXIT);
	XA STRQ  = _COLON("$\"|", DOSTR, EXIT);
	   DOTQ  = _COLON(".\"|", DOSTR, COUNT, TYPE, EXIT);
	XA DOTR  = _COLON(".R",   TOR, STR, RFROM, OVER, SUB, SPACS, TYPE, EXIT);
	XA UDOTR = _COLON("U.R",  TOR, BDIGS, DIGS, EDIGS, RFROM, OVER, SUB, SPACS, TYPE, EXIT);
	XA UDOT  = _COLON("U.",   BDIGS, DIGS, EDIGS, SPACE, TYPE, EXIT);
	XA DOT   = _COLON(".",    vBASE, AT, DOLIT, 0xa, XOR); {
		_IF(UDOT, EXIT);                    // base 10
		_THEN(STR, SPACE, TYPE, EXIT);      // other
	}
	XA QUEST = _COLON("?", AT, DOT, EXIT);
	//
	// Parser
    //
	XA PARSE0= _COLON("(parse)", vTEMP, CSTOR, OVER, TOR, DUP); {  // delimiter kept in vTEMP
		_IF(ONEM, vTEMP, CAT, BLANK, EQUAL); {                     // check <SPC>
			_IF(NOP); {
                // a FOR..WHILE..NEXT..THEN construct =~ for {..break..}
				_FOR(BLANK, OVER, CAT, SUB, ZLESS, INVER);    // 
                _WHILE(ONEP);                                 // break to THEN if is char, or next char
                _NEXT(RFROM, DROP, DOLIT, 0, DUP, EXIT);      // no break, (R>, DROP to rm loop counter)
                _THEN(RFROM);                                 // populate A0, i.e. break comes here, rm counter
            }
            _THEN(OVER, SWAP);                                // advance until next space found
            // a FOR..WHILE..NEXT..ELSE..THEN construct =~ DO..LEAVE..+LOOP
            _FOR(vTEMP, CAT, OVER, CAT, SUB, vTEMP, CAT, BLANK, EQUAL); {
                _IF(ZLESS);
                _THEN(NOP);
            }
            _WHILE(ONEP);                                     // if (char <= space) break to ELSE 
            _NEXT(DUP, TOR);                                  // no break, if counter < limit loop back to FOR
            _ELSE(RFROM, DROP, DUP, ONEP, TOR);               // R>, DROP to rm loop counter
            _THEN(OVER, SUB, RFROM, RFROM, SUB, EXIT);        // put token length on stack
        }
		_THEN(OVER, RFROM, SUB, EXIT);
	}
	XA PACKS = _COLON("PACK$", DUP, TOR, DDUP, CSTOR, ONEP, SWAP, CMOVE, RFROM, EXIT);
	XA PARSE = _COLON("PARSE",
					   TOR, TIB, vIN, AT, PLUS, vNTIB, AT, vIN, AT, SUB, RFROM,
					   PARSE0, vIN, PSTOR,
					   EXIT);
	XA TOKEN = _COLON("TOKEN", BLANK, PARSE, DOLIT, 0x1f, MIN, HERE, CELLP, PACKS, EXIT);  // put token at HERE
	XA WORD  = _COLON("WORD",  PARSE, HERE, CELLP, PACKS, EXIT);
	XA NAMET = _COLON("NAME>", COUNT, DOLIT, 0x1f, AND, PLUS, EXIT);
	XA SAMEQ = _COLON("SAME?", NOP); {  // (a1 a2 n - a1 a2 f) compare a1, a2 byte-by-byte, return 0 if same
        _FOR(DDUP);
#if CASE_SENSITIVE
        _AFT(DUP, CAT, TOR, ONEP, SWAP,                                    // *a1++
             DUP, CAT, TOR, ONEP, SWAP, RFROM, RFROM, SUB, QDUP); {        // *a2++
#else
        _AFT(DUP, CAT, TOUPP, TOR, ONEP, SWAP,                             // *a1++
             DUP, CAT, TOUPP, TOR, ONEP, SWAP, RFROM, RFROM, SUB, QDUP); { // *a2++
#endif // CASE_SENSITIVE
            _IF(RFROM, DROP, TOR, DDROP, RFROM, EXIT);                     // pop off loop counter and pointers
            _THEN(NOP);
        }
        _THEN(NOP);
        _NEXT(DDROP, DOLIT, 0, EXIT);
	}
	XA FIND = _COLON("find", SWAP, DUP, CAT, vTEMP, STORE,                 // keep length in temp
                     DUP, AT, TOR, CELLP, SWAP); {                         // fetch 1st cell
		_BEGIN(AT, DUP); {                                                 // 0000 = end of dic
#if CASE_SENSITIVE
			_IF(DUP, AT, DOLIT, 0xff3f, AND, RAT, XOR); {                  // compare 2-byte
#else
			_IF(DUP, AT, DOLIT, 0xff3f, AND, UPPER, RAT, UPPER, XOR); {    // compare 2-byte
#endif // CASE_SENSITIVE
				_IF(CELLP, DOLIT, 0xffff);                                 // miss, try next word
				_ELSE(CELLP, vTEMP, AT, ONEM, DUP); {                      // -1, since 1st byte has been compared
                    _IF(SAMEQ);                                            // compare strings if larger than 2 bytes
                    _THEN(NOP);
                }
				_THEN(NOP);
			}
			_ELSE(RFROM, DROP, SWAP, CELLM, SWAP, EXIT);
			_THEN(NOP);
		}
		_WHILE(CELLM, CELLM);                                             // get thread field to previous word
		_REPEAT(RFROM, DROP, SWAP, DROP, CELLM, DUP, NAMET, SWAP, EXIT);  // word found, get name field
	}
	XA NAMEQ = _COLON("NAME?", vCNTX, FIND, EXIT);
	//
	// Interpreter Input String handler
	//
	XA TAP   = _COLON("TAP", DUP, EMIT, OVER, CSTOR, ONEP, EXIT);                  // echo and store new char to TIB
	XA KTAP  = _COLON("kTAP", DUP, DOLIT, 0xd, XOR, OVER, DOLIT, 0xa, XOR, AND); { // check <CR><LF>
		_IF(DOLIT, 8, XOR); {                                                      // check <TAB>
			_IF(BLANK, TAP);                                                       // check BLANK
			_ELSE(HATH);
			_THEN(EXIT);
		}
		_THEN(DROP, SWAP, DROP, DUP, EXIT);
	}
	XA ACCEP = _COLON("ACCEPT", OVER, PLUS, OVER); {            // accquire token from console 
		_BEGIN(DDUP, XOR);                                      // loop through input stream
		_WHILE(KEY, DUP, BLANK, SUB, DOLIT, 0x5f, ULESS); {
			_IF(TAP);                                           // store new char into TIB
			_ELSE(KTAP);                                        // check if done
			_THEN(NOP);
		}
		_REPEAT(DROP, OVER, SUB, EXIT);                         // keep token length in #TIB
	}
	XA EXPEC = _COLON("EXPECT", ACCEP, vSPAN, STORE, DROP, EXIT);
	XA QUERY = _COLON("QUERY", TIB, DOLIT, FORTH_TIB_SZ, ACCEP,
                      vNTIB, STORE, DROP, DOLIT, 0, vIN, STORE, EXIT);
	//
	// Text Interpreter
	//
	/* QUIT Forth main interpreter loop
	   QUERY/TIB/ACCEPT - start the interpreter loop <-------<------.
	   TOKEN/PARSE - get a space delimited word                      \
	   @EXECUTE - attempt to look up that word in the dictionary      \
	   NAME?/find - was the word found?                                ^
	   |-Yes:                                                          |
	   |   $INTERPRET - are we in compile mode?                        |
	   |   |-Yes:                                                      ^
	   |   | \-Is the Word an Immediate word?                          |
	   |   |   |-Yes:                                                  |
	   |   |   | \- EXECUTE Execute the word ------------------->----->.
	   |   |   \-No:                                                   |
	   |   |     \-Compile the word into the dictionary -------->----->.
	   |   \-No:                                                       |
	   |     \- EXECUTE Execute the word ----->----------------->----->.
	   \-No:                                                           ^
	       NUMBER? - Can the word be treated as a number?              |
	       |-Yes:                                                      |
    	   | \-Are we in compile mode?                                 |
	       |   |-Yes:                                                  |
    	   |   | \-Compile a literal into the dictionary >------>----->.
	       |   \-No:                                                   |
    	   |     \-Push the number to the variable stack >------>----->.
    	   \-No:                                                      /
	        \-An Error has occurred, prXA out an error message >---->
	*/
	XA ATEXE = _COLON("@EXECUTE", AT, QDUP); {
		_IF(EXECU);
		_THEN(EXIT);
	}
	XA ABORT  = _COLON("ABORT", vTABRT, ATEXE);
	   ABORTQ = _COLON("abort\"", NOP); {
		_IF(DOSTR, COUNT, TYPE, ABORT);
		_THEN(DOSTR, DROP, EXIT);
	}
	XA ERROR = _COLON("ERROR", SPACE, COUNT, TYPE, DOLIT, 0x3f, EMIT, DOLIT, 0x1b, EMIT, CR, ABORT);
	XA INTER = _COLON("$INTERPRET", NAMEQ, QDUP); {  // scan dictionary for word
		_IF(CAT, DOLIT, fCOMPO, AND); {              // if it is compile only word
            _ABORTQ(" compile only");
            _LABEL(EXECU, EXIT);
        }
		_THEN(NUMBQ);                                // word name not found, check if it is a number
		_IF(EXIT);
		_ELSE(ERROR);
		_THEN(NOP);
	}
	XA iLBRAC = _IMMED("[", DOLIT, INTER, vTEVL, STORE, EXIT);
	XA DOTOK  = _COLON(".OK", CR, DOLIT, INTER, vTEVL, AT, EQUAL); {  // are we in interpreter mode?
		_IF(DEPTH, DOLIT, 3, MIN); {
			_FOR(RAT, PICK, DOT);
			_NEXT(NOP);
			_DOTQ(" ok>");
		}
		_THEN(EXIT);
	}
	XA EVAL  = _COLON("EVAL", NOP); {
		_BEGIN(TOKEN, DUP, CAT);  // fetch token length
		_WHILE(vTEVL, ATEXE);
		_REPEAT(DROP, DOTOK, EXIT);
	}
 	XA QUIT  = _COLON("QUIT", DOLIT, FORTH_TIB_ADDR, vTTIB, STORE, iLBRAC); {  // clear TIB, interpreter mode
		_BEGIN(QUERY, EVAL);      // main query-eval loop
		_AGAIN(NOP);
	}
	//
	// Forth Compiler - utility functions
	//
	XA ONLY  = _COLON("COMPILE-ONLY", DOLIT, fCOMPO, vLAST, AT, PSTOR, EXIT);  // enable COMPILE-ONLY flag
	XA IMMED = _COLON("IMMEDIATE",    DOLIT, fIMMED, vLAST, AT, PSTOR, EXIT);  // enable IMMEDIATE flag
	XA COMMA = _COLON(",",       HERE, DUP, CELLP, vCP, STORE, STORE, EXIT);   // store a byte
	XA CCMMA = _COLON("C,",      HERE, DUP, ONEP,  vCP, STORE, CSTOR, EXIT);   // store a word
	XA ALLOT = _COLON("ALLOT",   vCP, PSTOR, EXIT);
	XA UNIQU = _COLON("?UNIQUE", DUP, NAMEQ, QDUP); {
		_IF(COUNT, DOLIT, 0x1f, AND, SPACE, TYPE); {
			_DOTQ(" reDef");
		}
		_THEN(DROP, EXIT);
	}
	XA SNAME = _COLON("$,n", DUP, AT); {     // add new name field which is already build by PACK$
		_IF(UNIQU, DUP, NAMET, vCP, STORE, DUP, vLAST, STORE, CELLM, vCNTX, AT, SWAP, STORE, EXIT);
		_THEN(ERROR);
	}
	XA TICK  = _COLON("'", TOKEN, NAMEQ); {
		_IF(EXIT);
		_THEN(ERROR);
	}
	XA iLITR  = _IMMED("LITERAL", DOLIT, DOLIT, COMMA, COMMA, EXIT);    // create a literal
	XA iBCMP = _IMMED("[COMPILE]", TICK, COMMA, EXIT);                  // add word address to dictionary
	XA COMPI = _COLON("COMPILE",  RFROM, DUP, AT, COMMA, CELLP, TOR, EXIT);
	XA SCOMP = _COLON("$COMPILE", NAMEQ, QDUP); { // name found?
		_IF(AT, DOLIT, fIMMED, AND); {            // is immediate?
			_IF(EXECU);                           // execute
			_ELSE(COMMA);                         // or, add to dictionary
			_THEN(EXIT);
		}
		_THEN(NUMBQ);                             // a number?
		_IF(iLITR, EXIT);                         // add as literal
		_THEN(ERROR);
	}
	//
	// Forth Compiler - define new word
	//
	XA OVERT = _COLON("OVERT", vLAST, AT, vCNTX, STORE, EXIT);            // update LAST and CONTEXT variables
	XA RBRAC = _COLON("]", DOLIT, SCOMP, vTEVL, STORE, EXIT);             // switch into compiler-mode
	XA COLON = _COLON(":", TOKEN, SNAME, RBRAC, DOLIT, opENTER, CCMMA, EXIT);
	XA iSEMI = _IMMED(";", DOLIT, EXIT, COMMA, iLBRAC, OVERT, EXIT);
	XA FORGT = _COLON("FORGET", TOKEN, NAMEQ, QDUP); {
		_IF(CELLM, DUP, vCP, STORE, AT, DUP, vCNTX, STORE, vLAST, STORE, DROP, EXIT);
		_THEN(ERROR);
	}
	//
	// Forth Compiler - variable, constant, and comments
	//
	XA CODE   = _COLON("CODE",    TOKEN, SNAME, OVERT, EXIT);
	XA CREAT  = _COLON("CREATE",  CODE,  DOLIT, opDOVAR, CCMMA, EXIT);
	XA VARIA  = _COLON("VARIABLE",CREAT, DOLIT, 0, COMMA, EXIT);
	XA CONST  = _COLON("CONSTANT",CODE,  DOLIT, opDOCON, CCMMA, COMMA, EXIT);
	XA iDOTPR = _IMMED(".(",      DOLIT, 0x29, PARSE, TYPE, EXIT);              // print til hit ) i.e. 0x29
	XA iBKSLA = _IMMED("\\",      DOLIT, 0xa,  WORD,  DROP,  EXIT);             // skip til end of line
	XA iPAREN = _IMMED("(",       DOLIT, 0x29, PARSE, DDROP, EXIT);             // skip til )
	//
	// Forth Compiler - branching instructions
	//
    // BEGIN...AGAIN, BEGIN... f UNTIL, BEGIN...(once)...f WHILE...(loop)...REPEAT
    //
	XA iAHEAD = _IMMED("AHEAD",   COMPI, BRAN,  HERE, DOLIT, 0, COMMA, EXIT);
	XA iBEGIN = _IMMED("BEGIN",   HERE, EXIT);
	XA iAGAIN = _IMMED("AGAIN",   COMPI, BRAN,  COMMA, EXIT);
	XA iUNTIL = _IMMED("UNTIL",   COMPI, QBRAN, COMMA, EXIT);
    //
    // f IF...THEN, f IF...ELSE...THEN
    //
	XA iIF    = _IMMED("IF",      COMPI, QBRAN, HERE, DOLIT, 0, COMMA, EXIT);
	XA iTHEN  = _IMMED("THEN",    HERE, SWAP, STORE, EXIT);
	XA iELSE  = _IMMED("ELSE",    iAHEAD, SWAP, iTHEN, EXIT);
	XA iWHILE = _IMMED("WHILE",   iIF, SWAP, EXIT);
	XA iWHEN  = _IMMED("WHEN",    iIF, OVER, EXIT);
	XA iREPEA = _IMMED("REPEAT",  iAGAIN, iTHEN, EXIT);
    //
    // n FOR...NEXT, n FOR...(first)... f AFT...(2nd,...)...THEN...(every)...NEXT
    //
	XA iFOR   = _IMMED("FOR",     COMPI, TOR, HERE, EXIT);
	XA iAFT   = _IMMED("AFT",     DROP, iAHEAD, HERE, SWAP, EXIT);
	XA iNEXT  = _IMMED("NEXT",    COMPI, DONXT, COMMA, EXIT);
	//
	// Forth Compiler - String specification
	//
	XA STRCQ  = _COLON("$,\"",    DOLIT, 0x22, WORD,     // find quote in TIB (0x22 is " in ASCII)
                       COUNT, PLUS, vCP, STORE, EXIT);   // advance dic pointer
	XA iABRTQ = _IMMED("ABORT\"", DOLIT, ABORTQ, HERE, STORE, STRCQ, EXIT);
	XA iSTRQ  = _IMMED("$\"",     DOLIT, STRQ, HERE, STORE, STRCQ, EXIT);
	XA iDOTQ  = _IMMED(".\"",     DOLIT, DOTQ, HERE, STORE, STRCQ, EXIT);
	//
	// Debugging Tools
	//
	XA DMP   = _COLON("dm+", OVER, DOLIT, 5, UDOTR); {   // dump one row
		_FOR(DOLIT, 0x3a, EMIT);
		_AFT(DUP, AT, DOLIT, 5, UDOTR, CELLP);
		_THEN(NOP);
		_NEXT(EXIT);
	}
	XA DUMP  = _COLON("DUMP", vBASE, AT, TOR, HEX_, DOLIT, 0x1f, PLUS, DOLIT, 0x10, SLASH); {
		_FOR(NOP);
		_AFT(CR, DOLIT, 8, DDUP, DMP, TOR, SPACE, SPACE, CELLS, TYPE, RFROM);
		_THEN(NOP);
		_NEXT(DROP, RFROM, vBASE, STORE, EXIT);         // restore BASE
	}
	XA TNAME = _COLON(">NAME", NOP); {
		_BEGIN(ONEM, DUP, CAT, DOLIT, 0x7f, AND, DOLIT, 0x20, LESS);
		_UNTIL(EXIT);
	}
	XA DOTID = _COLON(".ID",   COUNT, DOLIT, 0x1f, AND, TYPE, SPACE, EXIT);
	XA WORDS = _COLON("WORDS", CR, vCNTX, DOLIT, 0, vTEMP, STORE); {
		_BEGIN(AT, QDUP);
		_WHILE(DUP, SPACE, DOTID, CELLM, vTEMP, AT, DOLIT, 0xa, LESS); {
			_IF(DOLIT, 1, vTEMP, PSTOR);
			_ELSE(CR, DOLIT, 0, vTEMP, STORE);
			_THEN(NOP);
		}
		_REPEAT(EXIT);
	}
    //
    // Arduino specific opcodes
    //
    XA DELAY = _CODE("DELAY",   opDELAY  );
    XA CLOCK = _CODE("CLOCK",   opCLOCK  );
	XA PIN   = _CODE("PINMODE", opPIN    );
	XA MAP   = _CODE("MAP",     opMAP    );
    XA DIN   = _CODE("IN",      opIN     );
    XA DOUT  = _CODE("OUT",     opOUT    );
    XA AIN   = _CODE("AIN",     opAIN    );
	XA AOUT  = _CODE("PWM",     opPWM    );
    //
    // End of dictionary (cold start here)
    //
	int last  = aPC + CELLSZ;               // name field of last word
	XA  COLD  = _COLON("COLD",
			DOLIT, last,  vCNTX, STORE,     // reset vectors
			DOLIT, last,  vLAST, STORE,
			DOLIT, INTER, vTEVL, STORE,
			DOLIT, QUIT,  vTABRT,STORE,
			CR, QUIT);   					// enter the main query loop (QUIT)
	int here  = aPC;                        // current pointer
	//
	// Setup Boot Vector
	//
	SET(FORTH_BOOT_ADDR+1, COLD);

	return here;
}

void ef_dump_rom(U8* cdata, int len)
{
    printf("//\n// cut and paste the following segment into Arduino C code\n//");
	printf("\nconst U32 forth_rom[] PROGMEM = {\n");
    for (int p=0; p<len+0x20; p+=0x20) {
        U32 *x = (U32*)&cdata[p];
        for (int i=0; i<0x8; i++, x++) {
            printf("0x%08x,", *x);
        }
        printf(" // %04x ", p);
        for (int i=0; i<0x20; i++) {
            U8 c = cdata[p+i];
            printf("%c", c ? ((c!=0x5c && c>0x1f && c<0x7f) ? c : '_') : '.');
        }
        printf("\n");
    }
    printf("};\n");
}

