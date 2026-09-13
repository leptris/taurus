/* html/html_parse.c — HTML parsing mode (#659, TODO.xslt-full/14).
 *
 * Tolerant HTML4/5 parse into the STANDARD DOM: same nodes, same
 * pool, same serializer as XML — no parallel tree (SSOT). Behaviors
 * follow libxml2's HTMLparser as Nokogiri exposes it: implied end
 * tags (p/li/td/tr/th/dt/dd/option/...), void elements, raw-text
 * script/style, minimized + unquoted attributes, case-insensitive
 * tag/attribute names, HTML named entities. With no explicit
 * <html>, the Nokogiri document shape is synthesized: <html><body>
 * (no <head> unless head content arrives; no implied <tbody>).
 *
 * The tokenizer never fails: malformed input degrades to text or
 * is dropped, and the builder closes open elements at EOF. */
#include "../leptris_internal.h"
#include "../dom/element.h"
#include "../dom/text.h"
#include "../dom/comment.h"
#include "../dom/doctype.h"
#include "../dom/root_doc_map.h"
#include "../dom/cdata.h"
#include "../dom/pi.h"
#include <stdint.h>
#include <string.h>

/* HTML named character references, single-codepoint set
 * (generated from the WHATWG list; HTML4 252-set + common
 * punctuation). Names are matched case-SENSITIVELY. */
typedef struct { const char* name; uint32_t cp; } HtmlEnt;
static const HtmlEnt k_html_entities[] = {
    {"AElig", 0x000C6},
    {"AMP", 0x00026},
    {"Aacute", 0x000C1},
    {"Abreve", 0x00102},
    {"Acirc", 0x000C2},
    {"Acy", 0x00410},
    {"Afr", 0x1D504},
    {"Agrave", 0x000C0},
    {"Alpha", 0x00391},
    {"Amacr", 0x00100},
    {"And", 0x02A53},
    {"Aogon", 0x00104},
    {"Aopf", 0x1D538},
    {"ApplyFunction", 0x02061},
    {"Aring", 0x000C5},
    {"Ascr", 0x1D49C},
    {"Assign", 0x02254},
    {"Atilde", 0x000C3},
    {"Auml", 0x000C4},
    {"Backslash", 0x02216},
    {"Barv", 0x02AE7},
    {"Barwed", 0x02306},
    {"Bcy", 0x00411},
    {"Because", 0x02235},
    {"Bernoullis", 0x0212C},
    {"Beta", 0x00392},
    {"Bfr", 0x1D505},
    {"Bopf", 0x1D539},
    {"Breve", 0x002D8},
    {"Bscr", 0x0212C},
    {"Bumpeq", 0x0224E},
    {"CHcy", 0x00427},
    {"COPY", 0x000A9},
    {"Cacute", 0x00106},
    {"Cap", 0x022D2},
    {"CapitalDifferentialD", 0x02145},
    {"Cayleys", 0x0212D},
    {"Ccaron", 0x0010C},
    {"Ccedil", 0x000C7},
    {"Ccirc", 0x00108},
    {"Cconint", 0x02230},
    {"Cdot", 0x0010A},
    {"Cedilla", 0x000B8},
    {"CenterDot", 0x000B7},
    {"Cfr", 0x0212D},
    {"Chi", 0x003A7},
    {"CircleDot", 0x02299},
    {"CircleMinus", 0x02296},
    {"CirclePlus", 0x02295},
    {"CircleTimes", 0x02297},
    {"ClockwiseContourIntegral", 0x02232},
    {"CloseCurlyDoubleQuote", 0x0201D},
    {"CloseCurlyQuote", 0x02019},
    {"Colon", 0x02237},
    {"Colone", 0x02A74},
    {"Congruent", 0x02261},
    {"Conint", 0x0222F},
    {"ContourIntegral", 0x0222E},
    {"Copf", 0x02102},
    {"Coproduct", 0x02210},
    {"CounterClockwiseContourIntegral", 0x02233},
    {"Cross", 0x02A2F},
    {"Cscr", 0x1D49E},
    {"Cup", 0x022D3},
    {"CupCap", 0x0224D},
    {"DD", 0x02145},
    {"DDotrahd", 0x02911},
    {"DJcy", 0x00402},
    {"DScy", 0x00405},
    {"DZcy", 0x0040F},
    {"Dagger", 0x02021},
    {"Darr", 0x021A1},
    {"Dashv", 0x02AE4},
    {"Dcaron", 0x0010E},
    {"Dcy", 0x00414},
    {"Del", 0x02207},
    {"Delta", 0x00394},
    {"Dfr", 0x1D507},
    {"DiacriticalAcute", 0x000B4},
    {"DiacriticalDot", 0x002D9},
    {"DiacriticalDoubleAcute", 0x002DD},
    {"DiacriticalGrave", 0x00060},
    {"DiacriticalTilde", 0x002DC},
    {"Diamond", 0x022C4},
    {"DifferentialD", 0x02146},
    {"Dopf", 0x1D53B},
    {"Dot", 0x000A8},
    {"DotDot", 0x020DC},
    {"DotEqual", 0x02250},
    {"DoubleContourIntegral", 0x0222F},
    {"DoubleDot", 0x000A8},
    {"DoubleDownArrow", 0x021D3},
    {"DoubleLeftArrow", 0x021D0},
    {"DoubleLeftRightArrow", 0x021D4},
    {"DoubleLeftTee", 0x02AE4},
    {"DoubleLongLeftArrow", 0x027F8},
    {"DoubleLongLeftRightArrow", 0x027FA},
    {"DoubleLongRightArrow", 0x027F9},
    {"DoubleRightArrow", 0x021D2},
    {"DoubleRightTee", 0x022A8},
    {"DoubleUpArrow", 0x021D1},
    {"DoubleUpDownArrow", 0x021D5},
    {"DoubleVerticalBar", 0x02225},
    {"DownArrow", 0x02193},
    {"DownArrowBar", 0x02913},
    {"DownArrowUpArrow", 0x021F5},
    {"DownBreve", 0x00311},
    {"DownLeftRightVector", 0x02950},
    {"DownLeftTeeVector", 0x0295E},
    {"DownLeftVector", 0x021BD},
    {"DownLeftVectorBar", 0x02956},
    {"DownRightTeeVector", 0x0295F},
    {"DownRightVector", 0x021C1},
    {"DownRightVectorBar", 0x02957},
    {"DownTee", 0x022A4},
    {"DownTeeArrow", 0x021A7},
    {"Downarrow", 0x021D3},
    {"Dscr", 0x1D49F},
    {"Dstrok", 0x00110},
    {"ENG", 0x0014A},
    {"ETH", 0x000D0},
    {"Eacute", 0x000C9},
    {"Ecaron", 0x0011A},
    {"Ecirc", 0x000CA},
    {"Ecy", 0x0042D},
    {"Edot", 0x00116},
    {"Efr", 0x1D508},
    {"Egrave", 0x000C8},
    {"Element", 0x02208},
    {"Emacr", 0x00112},
    {"EmptySmallSquare", 0x025FB},
    {"EmptyVerySmallSquare", 0x025AB},
    {"Eogon", 0x00118},
    {"Eopf", 0x1D53C},
    {"Epsilon", 0x00395},
    {"Equal", 0x02A75},
    {"EqualTilde", 0x02242},
    {"Equilibrium", 0x021CC},
    {"Escr", 0x02130},
    {"Esim", 0x02A73},
    {"Eta", 0x00397},
    {"Euml", 0x000CB},
    {"Exists", 0x02203},
    {"ExponentialE", 0x02147},
    {"Fcy", 0x00424},
    {"Ffr", 0x1D509},
    {"FilledSmallSquare", 0x025FC},
    {"FilledVerySmallSquare", 0x025AA},
    {"Fopf", 0x1D53D},
    {"ForAll", 0x02200},
    {"Fouriertrf", 0x02131},
    {"Fscr", 0x02131},
    {"GJcy", 0x00403},
    {"GT", 0x0003E},
    {"Gamma", 0x00393},
    {"Gammad", 0x003DC},
    {"Gbreve", 0x0011E},
    {"Gcedil", 0x00122},
    {"Gcirc", 0x0011C},
    {"Gcy", 0x00413},
    {"Gdot", 0x00120},
    {"Gfr", 0x1D50A},
    {"Gg", 0x022D9},
    {"Gopf", 0x1D53E},
    {"GreaterEqual", 0x02265},
    {"GreaterEqualLess", 0x022DB},
    {"GreaterFullEqual", 0x02267},
    {"GreaterGreater", 0x02AA2},
    {"GreaterLess", 0x02277},
    {"GreaterSlantEqual", 0x02A7E},
    {"GreaterTilde", 0x02273},
    {"Gscr", 0x1D4A2},
    {"Gt", 0x0226B},
    {"HARDcy", 0x0042A},
    {"Hacek", 0x002C7},
    {"Hat", 0x0005E},
    {"Hcirc", 0x00124},
    {"Hfr", 0x0210C},
    {"HilbertSpace", 0x0210B},
    {"Hopf", 0x0210D},
    {"HorizontalLine", 0x02500},
    {"Hscr", 0x0210B},
    {"Hstrok", 0x00126},
    {"HumpDownHump", 0x0224E},
    {"HumpEqual", 0x0224F},
    {"IEcy", 0x00415},
    {"IJlig", 0x00132},
    {"IOcy", 0x00401},
    {"Iacute", 0x000CD},
    {"Icirc", 0x000CE},
    {"Icy", 0x00418},
    {"Idot", 0x00130},
    {"Ifr", 0x02111},
    {"Igrave", 0x000CC},
    {"Im", 0x02111},
    {"Imacr", 0x0012A},
    {"ImaginaryI", 0x02148},
    {"Implies", 0x021D2},
    {"Int", 0x0222C},
    {"Integral", 0x0222B},
    {"Intersection", 0x022C2},
    {"InvisibleComma", 0x02063},
    {"InvisibleTimes", 0x02062},
    {"Iogon", 0x0012E},
    {"Iopf", 0x1D540},
    {"Iota", 0x00399},
    {"Iscr", 0x02110},
    {"Itilde", 0x00128},
    {"Iukcy", 0x00406},
    {"Iuml", 0x000CF},
    {"Jcirc", 0x00134},
    {"Jcy", 0x00419},
    {"Jfr", 0x1D50D},
    {"Jopf", 0x1D541},
    {"Jscr", 0x1D4A5},
    {"Jsercy", 0x00408},
    {"Jukcy", 0x00404},
    {"KHcy", 0x00425},
    {"KJcy", 0x0040C},
    {"Kappa", 0x0039A},
    {"Kcedil", 0x00136},
    {"Kcy", 0x0041A},
    {"Kfr", 0x1D50E},
    {"Kopf", 0x1D542},
    {"Kscr", 0x1D4A6},
    {"LJcy", 0x00409},
    {"LT", 0x0003C},
    {"Lacute", 0x00139},
    {"Lambda", 0x0039B},
    {"Lang", 0x027EA},
    {"Laplacetrf", 0x02112},
    {"Larr", 0x0219E},
    {"Lcaron", 0x0013D},
    {"Lcedil", 0x0013B},
    {"Lcy", 0x0041B},
    {"LeftAngleBracket", 0x027E8},
    {"LeftArrow", 0x02190},
    {"LeftArrowBar", 0x021E4},
    {"LeftArrowRightArrow", 0x021C6},
    {"LeftCeiling", 0x02308},
    {"LeftDoubleBracket", 0x027E6},
    {"LeftDownTeeVector", 0x02961},
    {"LeftDownVector", 0x021C3},
    {"LeftDownVectorBar", 0x02959},
    {"LeftFloor", 0x0230A},
    {"LeftRightArrow", 0x02194},
    {"LeftRightVector", 0x0294E},
    {"LeftTee", 0x022A3},
    {"LeftTeeArrow", 0x021A4},
    {"LeftTeeVector", 0x0295A},
    {"LeftTriangle", 0x022B2},
    {"LeftTriangleBar", 0x029CF},
    {"LeftTriangleEqual", 0x022B4},
    {"LeftUpDownVector", 0x02951},
    {"LeftUpTeeVector", 0x02960},
    {"LeftUpVector", 0x021BF},
    {"LeftUpVectorBar", 0x02958},
    {"LeftVector", 0x021BC},
    {"LeftVectorBar", 0x02952},
    {"Leftarrow", 0x021D0},
    {"Leftrightarrow", 0x021D4},
    {"LessEqualGreater", 0x022DA},
    {"LessFullEqual", 0x02266},
    {"LessGreater", 0x02276},
    {"LessLess", 0x02AA1},
    {"LessSlantEqual", 0x02A7D},
    {"LessTilde", 0x02272},
    {"Lfr", 0x1D50F},
    {"Ll", 0x022D8},
    {"Lleftarrow", 0x021DA},
    {"Lmidot", 0x0013F},
    {"LongLeftArrow", 0x027F5},
    {"LongLeftRightArrow", 0x027F7},
    {"LongRightArrow", 0x027F6},
    {"Longleftarrow", 0x027F8},
    {"Longleftrightarrow", 0x027FA},
    {"Longrightarrow", 0x027F9},
    {"Lopf", 0x1D543},
    {"LowerLeftArrow", 0x02199},
    {"LowerRightArrow", 0x02198},
    {"Lscr", 0x02112},
    {"Lsh", 0x021B0},
    {"Lstrok", 0x00141},
    {"Lt", 0x0226A},
    {"Map", 0x02905},
    {"Mcy", 0x0041C},
    {"MediumSpace", 0x0205F},
    {"Mellintrf", 0x02133},
    {"Mfr", 0x1D510},
    {"MinusPlus", 0x02213},
    {"Mopf", 0x1D544},
    {"Mscr", 0x02133},
    {"Mu", 0x0039C},
    {"NJcy", 0x0040A},
    {"Nacute", 0x00143},
    {"Ncaron", 0x00147},
    {"Ncedil", 0x00145},
    {"Ncy", 0x0041D},
    {"NegativeMediumSpace", 0x0200B},
    {"NegativeThickSpace", 0x0200B},
    {"NegativeThinSpace", 0x0200B},
    {"NegativeVeryThinSpace", 0x0200B},
    {"NestedGreaterGreater", 0x0226B},
    {"NestedLessLess", 0x0226A},
    {"NewLine", 0x0000A},
    {"Nfr", 0x1D511},
    {"NoBreak", 0x02060},
    {"NonBreakingSpace", 0x000A0},
    {"Nopf", 0x02115},
    {"Not", 0x02AEC},
    {"NotCongruent", 0x02262},
    {"NotCupCap", 0x0226D},
    {"NotDoubleVerticalBar", 0x02226},
    {"NotElement", 0x02209},
    {"NotEqual", 0x02260},
    {"NotExists", 0x02204},
    {"NotGreater", 0x0226F},
    {"NotGreaterEqual", 0x02271},
    {"NotGreaterLess", 0x02279},
    {"NotGreaterTilde", 0x02275},
    {"NotLeftTriangle", 0x022EA},
    {"NotLeftTriangleEqual", 0x022EC},
    {"NotLess", 0x0226E},
    {"NotLessEqual", 0x02270},
    {"NotLessGreater", 0x02278},
    {"NotLessTilde", 0x02274},
    {"NotPrecedes", 0x02280},
    {"NotPrecedesSlantEqual", 0x022E0},
    {"NotReverseElement", 0x0220C},
    {"NotRightTriangle", 0x022EB},
    {"NotRightTriangleEqual", 0x022ED},
    {"NotSquareSubsetEqual", 0x022E2},
    {"NotSquareSupersetEqual", 0x022E3},
    {"NotSubsetEqual", 0x02288},
    {"NotSucceeds", 0x02281},
    {"NotSucceedsSlantEqual", 0x022E1},
    {"NotSupersetEqual", 0x02289},
    {"NotTilde", 0x02241},
    {"NotTildeEqual", 0x02244},
    {"NotTildeFullEqual", 0x02247},
    {"NotTildeTilde", 0x02249},
    {"NotVerticalBar", 0x02224},
    {"Nscr", 0x1D4A9},
    {"Ntilde", 0x000D1},
    {"Nu", 0x0039D},
    {"OElig", 0x00152},
    {"Oacute", 0x000D3},
    {"Ocirc", 0x000D4},
    {"Ocy", 0x0041E},
    {"Odblac", 0x00150},
    {"Ofr", 0x1D512},
    {"Ograve", 0x000D2},
    {"Omacr", 0x0014C},
    {"Omega", 0x003A9},
    {"Omicron", 0x0039F},
    {"Oopf", 0x1D546},
    {"OpenCurlyDoubleQuote", 0x0201C},
    {"OpenCurlyQuote", 0x02018},
    {"Or", 0x02A54},
    {"Oscr", 0x1D4AA},
    {"Oslash", 0x000D8},
    {"Otilde", 0x000D5},
    {"Otimes", 0x02A37},
    {"Ouml", 0x000D6},
    {"OverBar", 0x0203E},
    {"OverBrace", 0x023DE},
    {"OverBracket", 0x023B4},
    {"OverParenthesis", 0x023DC},
    {"PartialD", 0x02202},
    {"Pcy", 0x0041F},
    {"Pfr", 0x1D513},
    {"Phi", 0x003A6},
    {"Pi", 0x003A0},
    {"PlusMinus", 0x000B1},
    {"Poincareplane", 0x0210C},
    {"Popf", 0x02119},
    {"Pr", 0x02ABB},
    {"Precedes", 0x0227A},
    {"PrecedesEqual", 0x02AAF},
    {"PrecedesSlantEqual", 0x0227C},
    {"PrecedesTilde", 0x0227E},
    {"Prime", 0x02033},
    {"Product", 0x0220F},
    {"Proportion", 0x02237},
    {"Proportional", 0x0221D},
    {"Pscr", 0x1D4AB},
    {"Psi", 0x003A8},
    {"QUOT", 0x00022},
    {"Qfr", 0x1D514},
    {"Qopf", 0x0211A},
    {"Qscr", 0x1D4AC},
    {"RBarr", 0x02910},
    {"REG", 0x000AE},
    {"Racute", 0x00154},
    {"Rang", 0x027EB},
    {"Rarr", 0x021A0},
    {"Rarrtl", 0x02916},
    {"Rcaron", 0x00158},
    {"Rcedil", 0x00156},
    {"Rcy", 0x00420},
    {"Re", 0x0211C},
    {"ReverseElement", 0x0220B},
    {"ReverseEquilibrium", 0x021CB},
    {"ReverseUpEquilibrium", 0x0296F},
    {"Rfr", 0x0211C},
    {"Rho", 0x003A1},
    {"RightAngleBracket", 0x027E9},
    {"RightArrow", 0x02192},
    {"RightArrowBar", 0x021E5},
    {"RightArrowLeftArrow", 0x021C4},
    {"RightCeiling", 0x02309},
    {"RightDoubleBracket", 0x027E7},
    {"RightDownTeeVector", 0x0295D},
    {"RightDownVector", 0x021C2},
    {"RightDownVectorBar", 0x02955},
    {"RightFloor", 0x0230B},
    {"RightTee", 0x022A2},
    {"RightTeeArrow", 0x021A6},
    {"RightTeeVector", 0x0295B},
    {"RightTriangle", 0x022B3},
    {"RightTriangleBar", 0x029D0},
    {"RightTriangleEqual", 0x022B5},
    {"RightUpDownVector", 0x0294F},
    {"RightUpTeeVector", 0x0295C},
    {"RightUpVector", 0x021BE},
    {"RightUpVectorBar", 0x02954},
    {"RightVector", 0x021C0},
    {"RightVectorBar", 0x02953},
    {"Rightarrow", 0x021D2},
    {"Ropf", 0x0211D},
    {"RoundImplies", 0x02970},
    {"Rrightarrow", 0x021DB},
    {"Rscr", 0x0211B},
    {"Rsh", 0x021B1},
    {"RuleDelayed", 0x029F4},
    {"SHCHcy", 0x00429},
    {"SHcy", 0x00428},
    {"SOFTcy", 0x0042C},
    {"Sacute", 0x0015A},
    {"Sc", 0x02ABC},
    {"Scaron", 0x00160},
    {"Scedil", 0x0015E},
    {"Scirc", 0x0015C},
    {"Scy", 0x00421},
    {"Sfr", 0x1D516},
    {"ShortDownArrow", 0x02193},
    {"ShortLeftArrow", 0x02190},
    {"ShortRightArrow", 0x02192},
    {"ShortUpArrow", 0x02191},
    {"Sigma", 0x003A3},
    {"SmallCircle", 0x02218},
    {"Sopf", 0x1D54A},
    {"Sqrt", 0x0221A},
    {"Square", 0x025A1},
    {"SquareIntersection", 0x02293},
    {"SquareSubset", 0x0228F},
    {"SquareSubsetEqual", 0x02291},
    {"SquareSuperset", 0x02290},
    {"SquareSupersetEqual", 0x02292},
    {"SquareUnion", 0x02294},
    {"Sscr", 0x1D4AE},
    {"Star", 0x022C6},
    {"Sub", 0x022D0},
    {"Subset", 0x022D0},
    {"SubsetEqual", 0x02286},
    {"Succeeds", 0x0227B},
    {"SucceedsEqual", 0x02AB0},
    {"SucceedsSlantEqual", 0x0227D},
    {"SucceedsTilde", 0x0227F},
    {"SuchThat", 0x0220B},
    {"Sum", 0x02211},
    {"Sup", 0x022D1},
    {"Superset", 0x02283},
    {"SupersetEqual", 0x02287},
    {"Supset", 0x022D1},
    {"THORN", 0x000DE},
    {"TRADE", 0x02122},
    {"TSHcy", 0x0040B},
    {"TScy", 0x00426},
    {"Tab", 0x00009},
    {"Tau", 0x003A4},
    {"Tcaron", 0x00164},
    {"Tcedil", 0x00162},
    {"Tcy", 0x00422},
    {"Tfr", 0x1D517},
    {"Therefore", 0x02234},
    {"Theta", 0x00398},
    {"ThinSpace", 0x02009},
    {"Tilde", 0x0223C},
    {"TildeEqual", 0x02243},
    {"TildeFullEqual", 0x02245},
    {"TildeTilde", 0x02248},
    {"Topf", 0x1D54B},
    {"TripleDot", 0x020DB},
    {"Tscr", 0x1D4AF},
    {"Tstrok", 0x00166},
    {"Uacute", 0x000DA},
    {"Uarr", 0x0219F},
    {"Uarrocir", 0x02949},
    {"Ubrcy", 0x0040E},
    {"Ubreve", 0x0016C},
    {"Ucirc", 0x000DB},
    {"Ucy", 0x00423},
    {"Udblac", 0x00170},
    {"Ufr", 0x1D518},
    {"Ugrave", 0x000D9},
    {"Umacr", 0x0016A},
    {"UnderBar", 0x0005F},
    {"UnderBrace", 0x023DF},
    {"UnderBracket", 0x023B5},
    {"UnderParenthesis", 0x023DD},
    {"Union", 0x022C3},
    {"UnionPlus", 0x0228E},
    {"Uogon", 0x00172},
    {"Uopf", 0x1D54C},
    {"UpArrow", 0x02191},
    {"UpArrowBar", 0x02912},
    {"UpArrowDownArrow", 0x021C5},
    {"UpDownArrow", 0x02195},
    {"UpEquilibrium", 0x0296E},
    {"UpTee", 0x022A5},
    {"UpTeeArrow", 0x021A5},
    {"Uparrow", 0x021D1},
    {"Updownarrow", 0x021D5},
    {"UpperLeftArrow", 0x02196},
    {"UpperRightArrow", 0x02197},
    {"Upsi", 0x003D2},
    {"Upsilon", 0x003A5},
    {"Uring", 0x0016E},
    {"Uscr", 0x1D4B0},
    {"Utilde", 0x00168},
    {"Uuml", 0x000DC},
    {"VDash", 0x022AB},
    {"Vbar", 0x02AEB},
    {"Vcy", 0x00412},
    {"Vdash", 0x022A9},
    {"Vdashl", 0x02AE6},
    {"Vee", 0x022C1},
    {"Verbar", 0x02016},
    {"Vert", 0x02016},
    {"VerticalBar", 0x02223},
    {"VerticalLine", 0x0007C},
    {"VerticalSeparator", 0x02758},
    {"VerticalTilde", 0x02240},
    {"VeryThinSpace", 0x0200A},
    {"Vfr", 0x1D519},
    {"Vopf", 0x1D54D},
    {"Vscr", 0x1D4B1},
    {"Vvdash", 0x022AA},
    {"Wcirc", 0x00174},
    {"Wedge", 0x022C0},
    {"Wfr", 0x1D51A},
    {"Wopf", 0x1D54E},
    {"Wscr", 0x1D4B2},
    {"Xfr", 0x1D51B},
    {"Xi", 0x0039E},
    {"Xopf", 0x1D54F},
    {"Xscr", 0x1D4B3},
    {"YAcy", 0x0042F},
    {"YIcy", 0x00407},
    {"YUcy", 0x0042E},
    {"Yacute", 0x000DD},
    {"Ycirc", 0x00176},
    {"Ycy", 0x0042B},
    {"Yfr", 0x1D51C},
    {"Yopf", 0x1D550},
    {"Yscr", 0x1D4B4},
    {"Yuml", 0x00178},
    {"ZHcy", 0x00416},
    {"Zacute", 0x00179},
    {"Zcaron", 0x0017D},
    {"Zcy", 0x00417},
    {"Zdot", 0x0017B},
    {"ZeroWidthSpace", 0x0200B},
    {"Zeta", 0x00396},
    {"Zfr", 0x02128},
    {"Zopf", 0x02124},
    {"Zscr", 0x1D4B5},
    {"aacute", 0x000E1},
    {"abreve", 0x00103},
    {"ac", 0x0223E},
    {"acd", 0x0223F},
    {"acirc", 0x000E2},
    {"acute", 0x000B4},
    {"acy", 0x00430},
    {"aelig", 0x000E6},
    {"af", 0x02061},
    {"afr", 0x1D51E},
    {"agrave", 0x000E0},
    {"alefsym", 0x02135},
    {"aleph", 0x02135},
    {"alpha", 0x003B1},
    {"amacr", 0x00101},
    {"amalg", 0x02A3F},
    {"amp", 0x00026},
    {"and", 0x02227},
    {"andand", 0x02A55},
    {"andd", 0x02A5C},
    {"andslope", 0x02A58},
    {"andv", 0x02A5A},
    {"ang", 0x02220},
    {"ange", 0x029A4},
    {"angle", 0x02220},
    {"angmsd", 0x02221},
    {"angmsdaa", 0x029A8},
    {"angmsdab", 0x029A9},
    {"angmsdac", 0x029AA},
    {"angmsdad", 0x029AB},
    {"angmsdae", 0x029AC},
    {"angmsdaf", 0x029AD},
    {"angmsdag", 0x029AE},
    {"angmsdah", 0x029AF},
    {"angrt", 0x0221F},
    {"angrtvb", 0x022BE},
    {"angrtvbd", 0x0299D},
    {"angsph", 0x02222},
    {"angst", 0x000C5},
    {"angzarr", 0x0237C},
    {"aogon", 0x00105},
    {"aopf", 0x1D552},
    {"ap", 0x02248},
    {"apE", 0x02A70},
    {"apacir", 0x02A6F},
    {"ape", 0x0224A},
    {"apid", 0x0224B},
    {"apos", 0x00027},
    {"approx", 0x02248},
    {"approxeq", 0x0224A},
    {"aring", 0x000E5},
    {"ascr", 0x1D4B6},
    {"ast", 0x0002A},
    {"asymp", 0x02248},
    {"asympeq", 0x0224D},
    {"atilde", 0x000E3},
    {"auml", 0x000E4},
    {"awconint", 0x02233},
    {"awint", 0x02A11},
    {"bNot", 0x02AED},
    {"backcong", 0x0224C},
    {"backepsilon", 0x003F6},
    {"backprime", 0x02035},
    {"backsim", 0x0223D},
    {"backsimeq", 0x022CD},
    {"barvee", 0x022BD},
    {"barwed", 0x02305},
    {"barwedge", 0x02305},
    {"bbrk", 0x023B5},
    {"bbrktbrk", 0x023B6},
    {"bcong", 0x0224C},
    {"bcy", 0x00431},
    {"bdquo", 0x0201E},
    {"becaus", 0x02235},
    {"because", 0x02235},
    {"bemptyv", 0x029B0},
    {"bepsi", 0x003F6},
    {"bernou", 0x0212C},
    {"beta", 0x003B2},
    {"beth", 0x02136},
    {"between", 0x0226C},
    {"bfr", 0x1D51F},
    {"bigcap", 0x022C2},
    {"bigcirc", 0x025EF},
    {"bigcup", 0x022C3},
    {"bigodot", 0x02A00},
    {"bigoplus", 0x02A01},
    {"bigotimes", 0x02A02},
    {"bigsqcup", 0x02A06},
    {"bigstar", 0x02605},
    {"bigtriangledown", 0x025BD},
    {"bigtriangleup", 0x025B3},
    {"biguplus", 0x02A04},
    {"bigvee", 0x022C1},
    {"bigwedge", 0x022C0},
    {"bkarow", 0x0290D},
    {"blacklozenge", 0x029EB},
    {"blacksquare", 0x025AA},
    {"blacktriangle", 0x025B4},
    {"blacktriangledown", 0x025BE},
    {"blacktriangleleft", 0x025C2},
    {"blacktriangleright", 0x025B8},
    {"blank", 0x02423},
    {"blk12", 0x02592},
    {"blk14", 0x02591},
    {"blk34", 0x02593},
    {"block", 0x02588},
    {"bnot", 0x02310},
    {"bopf", 0x1D553},
    {"bot", 0x022A5},
    {"bottom", 0x022A5},
    {"bowtie", 0x022C8},
    {"boxDL", 0x02557},
    {"boxDR", 0x02554},
    {"boxDl", 0x02556},
    {"boxDr", 0x02553},
    {"boxH", 0x02550},
    {"boxHD", 0x02566},
    {"boxHU", 0x02569},
    {"boxHd", 0x02564},
    {"boxHu", 0x02567},
    {"boxUL", 0x0255D},
    {"boxUR", 0x0255A},
    {"boxUl", 0x0255C},
    {"boxUr", 0x02559},
    {"boxV", 0x02551},
    {"boxVH", 0x0256C},
    {"boxVL", 0x02563},
    {"boxVR", 0x02560},
    {"boxVh", 0x0256B},
    {"boxVl", 0x02562},
    {"boxVr", 0x0255F},
    {"boxbox", 0x029C9},
    {"boxdL", 0x02555},
    {"boxdR", 0x02552},
    {"boxdl", 0x02510},
    {"boxdr", 0x0250C},
    {"boxh", 0x02500},
    {"boxhD", 0x02565},
    {"boxhU", 0x02568},
    {"boxhd", 0x0252C},
    {"boxhu", 0x02534},
    {"boxminus", 0x0229F},
    {"boxplus", 0x0229E},
    {"boxtimes", 0x022A0},
    {"boxuL", 0x0255B},
    {"boxuR", 0x02558},
    {"boxul", 0x02518},
    {"boxur", 0x02514},
    {"boxv", 0x02502},
    {"boxvH", 0x0256A},
    {"boxvL", 0x02561},
    {"boxvR", 0x0255E},
    {"boxvh", 0x0253C},
    {"boxvl", 0x02524},
    {"boxvr", 0x0251C},
    {"bprime", 0x02035},
    {"breve", 0x002D8},
    {"brvbar", 0x000A6},
    {"bscr", 0x1D4B7},
    {"bsemi", 0x0204F},
    {"bsim", 0x0223D},
    {"bsime", 0x022CD},
    {"bsol", 0x0005C},
    {"bsolb", 0x029C5},
    {"bsolhsub", 0x027C8},
    {"bull", 0x02022},
    {"bullet", 0x02022},
    {"bump", 0x0224E},
    {"bumpE", 0x02AAE},
    {"bumpe", 0x0224F},
    {"bumpeq", 0x0224F},
    {"cacute", 0x00107},
    {"cap", 0x02229},
    {"capand", 0x02A44},
    {"capbrcup", 0x02A49},
    {"capcap", 0x02A4B},
    {"capcup", 0x02A47},
    {"capdot", 0x02A40},
    {"caret", 0x02041},
    {"caron", 0x002C7},
    {"ccaps", 0x02A4D},
    {"ccaron", 0x0010D},
    {"ccedil", 0x000E7},
    {"ccirc", 0x00109},
    {"ccups", 0x02A4C},
    {"ccupssm", 0x02A50},
    {"cdot", 0x0010B},
    {"cedil", 0x000B8},
    {"cemptyv", 0x029B2},
    {"cent", 0x000A2},
    {"centerdot", 0x000B7},
    {"cfr", 0x1D520},
    {"chcy", 0x00447},
    {"check", 0x02713},
    {"checkmark", 0x02713},
    {"chi", 0x003C7},
    {"cir", 0x025CB},
    {"cirE", 0x029C3},
    {"circ", 0x002C6},
    {"circeq", 0x02257},
    {"circlearrowleft", 0x021BA},
    {"circlearrowright", 0x021BB},
    {"circledR", 0x000AE},
    {"circledS", 0x024C8},
    {"circledast", 0x0229B},
    {"circledcirc", 0x0229A},
    {"circleddash", 0x0229D},
    {"cire", 0x02257},
    {"cirfnint", 0x02A10},
    {"cirmid", 0x02AEF},
    {"cirscir", 0x029C2},
    {"clubs", 0x02663},
    {"clubsuit", 0x02663},
    {"colon", 0x0003A},
    {"colone", 0x02254},
    {"coloneq", 0x02254},
    {"comma", 0x0002C},
    {"commat", 0x00040},
    {"comp", 0x02201},
    {"compfn", 0x02218},
    {"complement", 0x02201},
    {"complexes", 0x02102},
    {"cong", 0x02245},
    {"congdot", 0x02A6D},
    {"conint", 0x0222E},
    {"copf", 0x1D554},
    {"coprod", 0x02210},
    {"copy", 0x000A9},
    {"copysr", 0x02117},
    {"crarr", 0x021B5},
    {"cross", 0x02717},
    {"cscr", 0x1D4B8},
    {"csub", 0x02ACF},
    {"csube", 0x02AD1},
    {"csup", 0x02AD0},
    {"csupe", 0x02AD2},
    {"ctdot", 0x022EF},
    {"cudarrl", 0x02938},
    {"cudarrr", 0x02935},
    {"cuepr", 0x022DE},
    {"cuesc", 0x022DF},
    {"cularr", 0x021B6},
    {"cularrp", 0x0293D},
    {"cup", 0x0222A},
    {"cupbrcap", 0x02A48},
    {"cupcap", 0x02A46},
    {"cupcup", 0x02A4A},
    {"cupdot", 0x0228D},
    {"cupor", 0x02A45},
    {"curarr", 0x021B7},
    {"curarrm", 0x0293C},
    {"curlyeqprec", 0x022DE},
    {"curlyeqsucc", 0x022DF},
    {"curlyvee", 0x022CE},
    {"curlywedge", 0x022CF},
    {"curren", 0x000A4},
    {"curvearrowleft", 0x021B6},
    {"curvearrowright", 0x021B7},
    {"cuvee", 0x022CE},
    {"cuwed", 0x022CF},
    {"cwconint", 0x02232},
    {"cwint", 0x02231},
    {"cylcty", 0x0232D},
    {"dArr", 0x021D3},
    {"dHar", 0x02965},
    {"dagger", 0x02020},
    {"daleth", 0x02138},
    {"darr", 0x02193},
    {"dash", 0x02010},
    {"dashv", 0x022A3},
    {"dbkarow", 0x0290F},
    {"dblac", 0x002DD},
    {"dcaron", 0x0010F},
    {"dcy", 0x00434},
    {"dd", 0x02146},
    {"ddagger", 0x02021},
    {"ddarr", 0x021CA},
    {"ddotseq", 0x02A77},
    {"deg", 0x000B0},
    {"delta", 0x003B4},
    {"demptyv", 0x029B1},
    {"dfisht", 0x0297F},
    {"dfr", 0x1D521},
    {"dharl", 0x021C3},
    {"dharr", 0x021C2},
    {"diam", 0x022C4},
    {"diamond", 0x022C4},
    {"diamondsuit", 0x02666},
    {"diams", 0x02666},
    {"die", 0x000A8},
    {"digamma", 0x003DD},
    {"disin", 0x022F2},
    {"div", 0x000F7},
    {"divide", 0x000F7},
    {"divideontimes", 0x022C7},
    {"divonx", 0x022C7},
    {"djcy", 0x00452},
    {"dlcorn", 0x0231E},
    {"dlcrop", 0x0230D},
    {"dollar", 0x00024},
    {"dopf", 0x1D555},
    {"dot", 0x002D9},
    {"doteq", 0x02250},
    {"doteqdot", 0x02251},
    {"dotminus", 0x02238},
    {"dotplus", 0x02214},
    {"dotsquare", 0x022A1},
    {"doublebarwedge", 0x02306},
    {"downarrow", 0x02193},
    {"downdownarrows", 0x021CA},
    {"downharpoonleft", 0x021C3},
    {"downharpoonright", 0x021C2},
    {"drbkarow", 0x02910},
    {"drcorn", 0x0231F},
    {"drcrop", 0x0230C},
    {"dscr", 0x1D4B9},
    {"dscy", 0x00455},
    {"dsol", 0x029F6},
    {"dstrok", 0x00111},
    {"dtdot", 0x022F1},
    {"dtri", 0x025BF},
    {"dtrif", 0x025BE},
    {"duarr", 0x021F5},
    {"duhar", 0x0296F},
    {"dwangle", 0x029A6},
    {"dzcy", 0x0045F},
    {"dzigrarr", 0x027FF},
    {"eDDot", 0x02A77},
    {"eDot", 0x02251},
    {"eacute", 0x000E9},
    {"easter", 0x02A6E},
    {"ecaron", 0x0011B},
    {"ecir", 0x02256},
    {"ecirc", 0x000EA},
    {"ecolon", 0x02255},
    {"ecy", 0x0044D},
    {"edot", 0x00117},
    {"ee", 0x02147},
    {"efDot", 0x02252},
    {"efr", 0x1D522},
    {"eg", 0x02A9A},
    {"egrave", 0x000E8},
    {"egs", 0x02A96},
    {"egsdot", 0x02A98},
    {"el", 0x02A99},
    {"elinters", 0x023E7},
    {"ell", 0x02113},
    {"els", 0x02A95},
    {"elsdot", 0x02A97},
    {"emacr", 0x00113},
    {"empty", 0x02205},
    {"emptyset", 0x02205},
    {"emptyv", 0x02205},
    {"emsp", 0x02003},
    {"emsp13", 0x02004},
    {"emsp14", 0x02005},
    {"eng", 0x0014B},
    {"ensp", 0x02002},
    {"eogon", 0x00119},
    {"eopf", 0x1D556},
    {"epar", 0x022D5},
    {"eparsl", 0x029E3},
    {"eplus", 0x02A71},
    {"epsi", 0x003B5},
    {"epsilon", 0x003B5},
    {"epsiv", 0x003F5},
    {"eqcirc", 0x02256},
    {"eqcolon", 0x02255},
    {"eqsim", 0x02242},
    {"eqslantgtr", 0x02A96},
    {"eqslantless", 0x02A95},
    {"equals", 0x0003D},
    {"equest", 0x0225F},
    {"equiv", 0x02261},
    {"equivDD", 0x02A78},
    {"eqvparsl", 0x029E5},
    {"erDot", 0x02253},
    {"erarr", 0x02971},
    {"escr", 0x0212F},
    {"esdot", 0x02250},
    {"esim", 0x02242},
    {"eta", 0x003B7},
    {"eth", 0x000F0},
    {"euml", 0x000EB},
    {"euro", 0x020AC},
    {"excl", 0x00021},
    {"exist", 0x02203},
    {"expectation", 0x02130},
    {"exponentiale", 0x02147},
    {"fallingdotseq", 0x02252},
    {"fcy", 0x00444},
    {"female", 0x02640},
    {"ffilig", 0x0FB03},
    {"fflig", 0x0FB00},
    {"ffllig", 0x0FB04},
    {"ffr", 0x1D523},
    {"filig", 0x0FB01},
    {"flat", 0x0266D},
    {"fllig", 0x0FB02},
    {"fltns", 0x025B1},
    {"fnof", 0x00192},
    {"fopf", 0x1D557},
    {"forall", 0x02200},
    {"fork", 0x022D4},
    {"forkv", 0x02AD9},
    {"fpartint", 0x02A0D},
    {"frac12", 0x000BD},
    {"frac13", 0x02153},
    {"frac14", 0x000BC},
    {"frac15", 0x02155},
    {"frac16", 0x02159},
    {"frac18", 0x0215B},
    {"frac23", 0x02154},
    {"frac25", 0x02156},
    {"frac34", 0x000BE},
    {"frac35", 0x02157},
    {"frac38", 0x0215C},
    {"frac45", 0x02158},
    {"frac56", 0x0215A},
    {"frac58", 0x0215D},
    {"frac78", 0x0215E},
    {"frasl", 0x02044},
    {"frown", 0x02322},
    {"fscr", 0x1D4BB},
    {"gE", 0x02267},
    {"gEl", 0x02A8C},
    {"gacute", 0x001F5},
    {"gamma", 0x003B3},
    {"gammad", 0x003DD},
    {"gap", 0x02A86},
    {"gbreve", 0x0011F},
    {"gcirc", 0x0011D},
    {"gcy", 0x00433},
    {"gdot", 0x00121},
    {"ge", 0x02265},
    {"gel", 0x022DB},
    {"geq", 0x02265},
    {"geqq", 0x02267},
    {"geqslant", 0x02A7E},
    {"ges", 0x02A7E},
    {"gescc", 0x02AA9},
    {"gesdot", 0x02A80},
    {"gesdoto", 0x02A82},
    {"gesdotol", 0x02A84},
    {"gesles", 0x02A94},
    {"gfr", 0x1D524},
    {"gg", 0x0226B},
    {"ggg", 0x022D9},
    {"gimel", 0x02137},
    {"gjcy", 0x00453},
    {"gl", 0x02277},
    {"glE", 0x02A92},
    {"gla", 0x02AA5},
    {"glj", 0x02AA4},
    {"gnE", 0x02269},
    {"gnap", 0x02A8A},
    {"gnapprox", 0x02A8A},
    {"gne", 0x02A88},
    {"gneq", 0x02A88},
    {"gneqq", 0x02269},
    {"gnsim", 0x022E7},
    {"gopf", 0x1D558},
    {"grave", 0x00060},
    {"gscr", 0x0210A},
    {"gsim", 0x02273},
    {"gsime", 0x02A8E},
    {"gsiml", 0x02A90},
    {"gt", 0x0003E},
    {"gtcc", 0x02AA7},
    {"gtcir", 0x02A7A},
    {"gtdot", 0x022D7},
    {"gtlPar", 0x02995},
    {"gtquest", 0x02A7C},
    {"gtrapprox", 0x02A86},
    {"gtrarr", 0x02978},
    {"gtrdot", 0x022D7},
    {"gtreqless", 0x022DB},
    {"gtreqqless", 0x02A8C},
    {"gtrless", 0x02277},
    {"gtrsim", 0x02273},
    {"hArr", 0x021D4},
    {"hairsp", 0x0200A},
    {"half", 0x000BD},
    {"hamilt", 0x0210B},
    {"hardcy", 0x0044A},
    {"harr", 0x02194},
    {"harrcir", 0x02948},
    {"harrw", 0x021AD},
    {"hbar", 0x0210F},
    {"hcirc", 0x00125},
    {"hearts", 0x02665},
    {"heartsuit", 0x02665},
    {"hellip", 0x02026},
    {"hercon", 0x022B9},
    {"hfr", 0x1D525},
    {"hksearow", 0x02925},
    {"hkswarow", 0x02926},
    {"hoarr", 0x021FF},
    {"homtht", 0x0223B},
    {"hookleftarrow", 0x021A9},
    {"hookrightarrow", 0x021AA},
    {"hopf", 0x1D559},
    {"horbar", 0x02015},
    {"hscr", 0x1D4BD},
    {"hslash", 0x0210F},
    {"hstrok", 0x00127},
    {"hybull", 0x02043},
    {"hyphen", 0x02010},
    {"iacute", 0x000ED},
    {"ic", 0x02063},
    {"icirc", 0x000EE},
    {"icy", 0x00438},
    {"iecy", 0x00435},
    {"iexcl", 0x000A1},
    {"iff", 0x021D4},
    {"ifr", 0x1D526},
    {"igrave", 0x000EC},
    {"ii", 0x02148},
    {"iiiint", 0x02A0C},
    {"iiint", 0x0222D},
    {"iinfin", 0x029DC},
    {"iiota", 0x02129},
    {"ijlig", 0x00133},
    {"imacr", 0x0012B},
    {"image", 0x02111},
    {"imagline", 0x02110},
    {"imagpart", 0x02111},
    {"imath", 0x00131},
    {"imof", 0x022B7},
    {"imped", 0x001B5},
    {"in", 0x02208},
    {"incare", 0x02105},
    {"infin", 0x0221E},
    {"infintie", 0x029DD},
    {"inodot", 0x00131},
    {"int", 0x0222B},
    {"intcal", 0x022BA},
    {"integers", 0x02124},
    {"intercal", 0x022BA},
    {"intlarhk", 0x02A17},
    {"intprod", 0x02A3C},
    {"iocy", 0x00451},
    {"iogon", 0x0012F},
    {"iopf", 0x1D55A},
    {"iota", 0x003B9},
    {"iprod", 0x02A3C},
    {"iquest", 0x000BF},
    {"iscr", 0x1D4BE},
    {"isin", 0x02208},
    {"isinE", 0x022F9},
    {"isindot", 0x022F5},
    {"isins", 0x022F4},
    {"isinsv", 0x022F3},
    {"isinv", 0x02208},
    {"it", 0x02062},
    {"itilde", 0x00129},
    {"iukcy", 0x00456},
    {"iuml", 0x000EF},
    {"jcirc", 0x00135},
    {"jcy", 0x00439},
    {"jfr", 0x1D527},
    {"jmath", 0x00237},
    {"jopf", 0x1D55B},
    {"jscr", 0x1D4BF},
    {"jsercy", 0x00458},
    {"jukcy", 0x00454},
    {"kappa", 0x003BA},
    {"kappav", 0x003F0},
    {"kcedil", 0x00137},
    {"kcy", 0x0043A},
    {"kfr", 0x1D528},
    {"kgreen", 0x00138},
    {"khcy", 0x00445},
    {"kjcy", 0x0045C},
    {"kopf", 0x1D55C},
    {"kscr", 0x1D4C0},
    {"lAarr", 0x021DA},
    {"lArr", 0x021D0},
    {"lAtail", 0x0291B},
    {"lBarr", 0x0290E},
    {"lE", 0x02266},
    {"lEg", 0x02A8B},
    {"lHar", 0x02962},
    {"lacute", 0x0013A},
    {"laemptyv", 0x029B4},
    {"lagran", 0x02112},
    {"lambda", 0x003BB},
    {"lang", 0x027E8},
    {"langd", 0x02991},
    {"langle", 0x027E8},
    {"lap", 0x02A85},
    {"laquo", 0x000AB},
    {"larr", 0x02190},
    {"larrb", 0x021E4},
    {"larrbfs", 0x0291F},
    {"larrfs", 0x0291D},
    {"larrhk", 0x021A9},
    {"larrlp", 0x021AB},
    {"larrpl", 0x02939},
    {"larrsim", 0x02973},
    {"larrtl", 0x021A2},
    {"lat", 0x02AAB},
    {"latail", 0x02919},
    {"late", 0x02AAD},
    {"lbarr", 0x0290C},
    {"lbbrk", 0x02772},
    {"lbrace", 0x0007B},
    {"lbrack", 0x0005B},
    {"lbrke", 0x0298B},
    {"lbrksld", 0x0298F},
    {"lbrkslu", 0x0298D},
    {"lcaron", 0x0013E},
    {"lcedil", 0x0013C},
    {"lceil", 0x02308},
    {"lcub", 0x0007B},
    {"lcy", 0x0043B},
    {"ldca", 0x02936},
    {"ldquo", 0x0201C},
    {"ldquor", 0x0201E},
    {"ldrdhar", 0x02967},
    {"ldrushar", 0x0294B},
    {"ldsh", 0x021B2},
    {"le", 0x02264},
    {"leftarrow", 0x02190},
    {"leftarrowtail", 0x021A2},
    {"leftharpoondown", 0x021BD},
    {"leftharpoonup", 0x021BC},
    {"leftleftarrows", 0x021C7},
    {"leftrightarrow", 0x02194},
    {"leftrightarrows", 0x021C6},
    {"leftrightharpoons", 0x021CB},
    {"leftrightsquigarrow", 0x021AD},
    {"leftthreetimes", 0x022CB},
    {"leg", 0x022DA},
    {"leq", 0x02264},
    {"leqq", 0x02266},
    {"leqslant", 0x02A7D},
    {"les", 0x02A7D},
    {"lescc", 0x02AA8},
    {"lesdot", 0x02A7F},
    {"lesdoto", 0x02A81},
    {"lesdotor", 0x02A83},
    {"lesges", 0x02A93},
    {"lessapprox", 0x02A85},
    {"lessdot", 0x022D6},
    {"lesseqgtr", 0x022DA},
    {"lesseqqgtr", 0x02A8B},
    {"lessgtr", 0x02276},
    {"lesssim", 0x02272},
    {"lfisht", 0x0297C},
    {"lfloor", 0x0230A},
    {"lfr", 0x1D529},
    {"lg", 0x02276},
    {"lgE", 0x02A91},
    {"lhard", 0x021BD},
    {"lharu", 0x021BC},
    {"lharul", 0x0296A},
    {"lhblk", 0x02584},
    {"ljcy", 0x00459},
    {"ll", 0x0226A},
    {"llarr", 0x021C7},
    {"llcorner", 0x0231E},
    {"llhard", 0x0296B},
    {"lltri", 0x025FA},
    {"lmidot", 0x00140},
    {"lmoust", 0x023B0},
    {"lmoustache", 0x023B0},
    {"lnE", 0x02268},
    {"lnap", 0x02A89},
    {"lnapprox", 0x02A89},
    {"lne", 0x02A87},
    {"lneq", 0x02A87},
    {"lneqq", 0x02268},
    {"lnsim", 0x022E6},
    {"loang", 0x027EC},
    {"loarr", 0x021FD},
    {"lobrk", 0x027E6},
    {"longleftarrow", 0x027F5},
    {"longleftrightarrow", 0x027F7},
    {"longmapsto", 0x027FC},
    {"longrightarrow", 0x027F6},
    {"looparrowleft", 0x021AB},
    {"looparrowright", 0x021AC},
    {"lopar", 0x02985},
    {"lopf", 0x1D55D},
    {"loplus", 0x02A2D},
    {"lotimes", 0x02A34},
    {"lowast", 0x02217},
    {"lowbar", 0x0005F},
    {"loz", 0x025CA},
    {"lozenge", 0x025CA},
    {"lozf", 0x029EB},
    {"lpar", 0x00028},
    {"lparlt", 0x02993},
    {"lrarr", 0x021C6},
    {"lrcorner", 0x0231F},
    {"lrhar", 0x021CB},
    {"lrhard", 0x0296D},
    {"lrm", 0x0200E},
    {"lrtri", 0x022BF},
    {"lsaquo", 0x02039},
    {"lscr", 0x1D4C1},
    {"lsh", 0x021B0},
    {"lsim", 0x02272},
    {"lsime", 0x02A8D},
    {"lsimg", 0x02A8F},
    {"lsqb", 0x0005B},
    {"lsquo", 0x02018},
    {"lsquor", 0x0201A},
    {"lstrok", 0x00142},
    {"lt", 0x0003C},
    {"ltcc", 0x02AA6},
    {"ltcir", 0x02A79},
    {"ltdot", 0x022D6},
    {"lthree", 0x022CB},
    {"ltimes", 0x022C9},
    {"ltlarr", 0x02976},
    {"ltquest", 0x02A7B},
    {"ltrPar", 0x02996},
    {"ltri", 0x025C3},
    {"ltrie", 0x022B4},
    {"ltrif", 0x025C2},
    {"lurdshar", 0x0294A},
    {"luruhar", 0x02966},
    {"mDDot", 0x0223A},
    {"macr", 0x000AF},
    {"male", 0x02642},
    {"malt", 0x02720},
    {"maltese", 0x02720},
    {"map", 0x021A6},
    {"mapsto", 0x021A6},
    {"mapstodown", 0x021A7},
    {"mapstoleft", 0x021A4},
    {"mapstoup", 0x021A5},
    {"marker", 0x025AE},
    {"mcomma", 0x02A29},
    {"mcy", 0x0043C},
    {"mdash", 0x02014},
    {"measuredangle", 0x02221},
    {"mfr", 0x1D52A},
    {"mho", 0x02127},
    {"micro", 0x000B5},
    {"mid", 0x02223},
    {"midast", 0x0002A},
    {"midcir", 0x02AF0},
    {"middot", 0x000B7},
    {"minus", 0x02212},
    {"minusb", 0x0229F},
    {"minusd", 0x02238},
    {"minusdu", 0x02A2A},
    {"mlcp", 0x02ADB},
    {"mldr", 0x02026},
    {"mnplus", 0x02213},
    {"models", 0x022A7},
    {"mopf", 0x1D55E},
    {"mp", 0x02213},
    {"mscr", 0x1D4C2},
    {"mstpos", 0x0223E},
    {"mu", 0x003BC},
    {"multimap", 0x022B8},
    {"mumap", 0x022B8},
    {"nLeftarrow", 0x021CD},
    {"nLeftrightarrow", 0x021CE},
    {"nRightarrow", 0x021CF},
    {"nVDash", 0x022AF},
    {"nVdash", 0x022AE},
    {"nabla", 0x02207},
    {"nacute", 0x00144},
    {"nap", 0x02249},
    {"napos", 0x00149},
    {"napprox", 0x02249},
    {"natur", 0x0266E},
    {"natural", 0x0266E},
    {"naturals", 0x02115},
    {"nbsp", 0x000A0},
    {"ncap", 0x02A43},
    {"ncaron", 0x00148},
    {"ncedil", 0x00146},
    {"ncong", 0x02247},
    {"ncup", 0x02A42},
    {"ncy", 0x0043D},
    {"ndash", 0x02013},
    {"ne", 0x02260},
    {"neArr", 0x021D7},
    {"nearhk", 0x02924},
    {"nearr", 0x02197},
    {"nearrow", 0x02197},
    {"nequiv", 0x02262},
    {"nesear", 0x02928},
    {"nexist", 0x02204},
    {"nexists", 0x02204},
    {"nfr", 0x1D52B},
    {"nge", 0x02271},
    {"ngeq", 0x02271},
    {"ngsim", 0x02275},
    {"ngt", 0x0226F},
    {"ngtr", 0x0226F},
    {"nhArr", 0x021CE},
    {"nharr", 0x021AE},
    {"nhpar", 0x02AF2},
    {"ni", 0x0220B},
    {"nis", 0x022FC},
    {"nisd", 0x022FA},
    {"niv", 0x0220B},
    {"njcy", 0x0045A},
    {"nlArr", 0x021CD},
    {"nlarr", 0x0219A},
    {"nldr", 0x02025},
    {"nle", 0x02270},
    {"nleftarrow", 0x0219A},
    {"nleftrightarrow", 0x021AE},
    {"nleq", 0x02270},
    {"nless", 0x0226E},
    {"nlsim", 0x02274},
    {"nlt", 0x0226E},
    {"nltri", 0x022EA},
    {"nltrie", 0x022EC},
    {"nmid", 0x02224},
    {"nopf", 0x1D55F},
    {"not", 0x000AC},
    {"notin", 0x02209},
    {"notinva", 0x02209},
    {"notinvb", 0x022F7},
    {"notinvc", 0x022F6},
    {"notni", 0x0220C},
    {"notniva", 0x0220C},
    {"notnivb", 0x022FE},
    {"notnivc", 0x022FD},
    {"npar", 0x02226},
    {"nparallel", 0x02226},
    {"npolint", 0x02A14},
    {"npr", 0x02280},
    {"nprcue", 0x022E0},
    {"nprec", 0x02280},
    {"nrArr", 0x021CF},
    {"nrarr", 0x0219B},
    {"nrightarrow", 0x0219B},
    {"nrtri", 0x022EB},
    {"nrtrie", 0x022ED},
    {"nsc", 0x02281},
    {"nsccue", 0x022E1},
    {"nscr", 0x1D4C3},
    {"nshortmid", 0x02224},
    {"nshortparallel", 0x02226},
    {"nsim", 0x02241},
    {"nsime", 0x02244},
    {"nsimeq", 0x02244},
    {"nsmid", 0x02224},
    {"nspar", 0x02226},
    {"nsqsube", 0x022E2},
    {"nsqsupe", 0x022E3},
    {"nsub", 0x02284},
    {"nsube", 0x02288},
    {"nsubseteq", 0x02288},
    {"nsucc", 0x02281},
    {"nsup", 0x02285},
    {"nsupe", 0x02289},
    {"nsupseteq", 0x02289},
    {"ntgl", 0x02279},
    {"ntilde", 0x000F1},
    {"ntlg", 0x02278},
    {"ntriangleleft", 0x022EA},
    {"ntrianglelefteq", 0x022EC},
    {"ntriangleright", 0x022EB},
    {"ntrianglerighteq", 0x022ED},
    {"nu", 0x003BD},
    {"num", 0x00023},
    {"numero", 0x02116},
    {"numsp", 0x02007},
    {"nvDash", 0x022AD},
    {"nvHarr", 0x02904},
    {"nvdash", 0x022AC},
    {"nvinfin", 0x029DE},
    {"nvlArr", 0x02902},
    {"nvrArr", 0x02903},
    {"nwArr", 0x021D6},
    {"nwarhk", 0x02923},
    {"nwarr", 0x02196},
    {"nwarrow", 0x02196},
    {"nwnear", 0x02927},
    {"oS", 0x024C8},
    {"oacute", 0x000F3},
    {"oast", 0x0229B},
    {"ocir", 0x0229A},
    {"ocirc", 0x000F4},
    {"ocy", 0x0043E},
    {"odash", 0x0229D},
    {"odblac", 0x00151},
    {"odiv", 0x02A38},
    {"odot", 0x02299},
    {"odsold", 0x029BC},
    {"oelig", 0x00153},
    {"ofcir", 0x029BF},
    {"ofr", 0x1D52C},
    {"ogon", 0x002DB},
    {"ograve", 0x000F2},
    {"ogt", 0x029C1},
    {"ohbar", 0x029B5},
    {"ohm", 0x003A9},
    {"oint", 0x0222E},
    {"olarr", 0x021BA},
    {"olcir", 0x029BE},
    {"olcross", 0x029BB},
    {"oline", 0x0203E},
    {"olt", 0x029C0},
    {"omacr", 0x0014D},
    {"omega", 0x003C9},
    {"omicron", 0x003BF},
    {"omid", 0x029B6},
    {"ominus", 0x02296},
    {"oopf", 0x1D560},
    {"opar", 0x029B7},
    {"operp", 0x029B9},
    {"oplus", 0x02295},
    {"or", 0x02228},
    {"orarr", 0x021BB},
    {"ord", 0x02A5D},
    {"order", 0x02134},
    {"orderof", 0x02134},
    {"ordf", 0x000AA},
    {"ordm", 0x000BA},
    {"origof", 0x022B6},
    {"oror", 0x02A56},
    {"orslope", 0x02A57},
    {"orv", 0x02A5B},
    {"oscr", 0x02134},
    {"oslash", 0x000F8},
    {"osol", 0x02298},
    {"otilde", 0x000F5},
    {"otimes", 0x02297},
    {"otimesas", 0x02A36},
    {"ouml", 0x000F6},
    {"ovbar", 0x0233D},
    {"par", 0x02225},
    {"para", 0x000B6},
    {"parallel", 0x02225},
    {"parsim", 0x02AF3},
    {"parsl", 0x02AFD},
    {"part", 0x02202},
    {"pcy", 0x0043F},
    {"percnt", 0x00025},
    {"period", 0x0002E},
    {"permil", 0x02030},
    {"perp", 0x022A5},
    {"pertenk", 0x02031},
    {"pfr", 0x1D52D},
    {"phi", 0x003C6},
    {"phiv", 0x003D5},
    {"phmmat", 0x02133},
    {"phone", 0x0260E},
    {"pi", 0x003C0},
    {"pitchfork", 0x022D4},
    {"piv", 0x003D6},
    {"planck", 0x0210F},
    {"planckh", 0x0210E},
    {"plankv", 0x0210F},
    {"plus", 0x0002B},
    {"plusacir", 0x02A23},
    {"plusb", 0x0229E},
    {"pluscir", 0x02A22},
    {"plusdo", 0x02214},
    {"plusdu", 0x02A25},
    {"pluse", 0x02A72},
    {"plusmn", 0x000B1},
    {"plussim", 0x02A26},
    {"plustwo", 0x02A27},
    {"pm", 0x000B1},
    {"pointint", 0x02A15},
    {"popf", 0x1D561},
    {"pound", 0x000A3},
    {"pr", 0x0227A},
    {"prE", 0x02AB3},
    {"prap", 0x02AB7},
    {"prcue", 0x0227C},
    {"pre", 0x02AAF},
    {"prec", 0x0227A},
    {"precapprox", 0x02AB7},
    {"preccurlyeq", 0x0227C},
    {"preceq", 0x02AAF},
    {"precnapprox", 0x02AB9},
    {"precneqq", 0x02AB5},
    {"precnsim", 0x022E8},
    {"precsim", 0x0227E},
    {"prime", 0x02032},
    {"primes", 0x02119},
    {"prnE", 0x02AB5},
    {"prnap", 0x02AB9},
    {"prnsim", 0x022E8},
    {"prod", 0x0220F},
    {"profalar", 0x0232E},
    {"profline", 0x02312},
    {"profsurf", 0x02313},
    {"prop", 0x0221D},
    {"propto", 0x0221D},
    {"prsim", 0x0227E},
    {"prurel", 0x022B0},
    {"pscr", 0x1D4C5},
    {"psi", 0x003C8},
    {"puncsp", 0x02008},
    {"qfr", 0x1D52E},
    {"qint", 0x02A0C},
    {"qopf", 0x1D562},
    {"qprime", 0x02057},
    {"qscr", 0x1D4C6},
    {"quaternions", 0x0210D},
    {"quatint", 0x02A16},
    {"quest", 0x0003F},
    {"questeq", 0x0225F},
    {"quot", 0x00022},
    {"rAarr", 0x021DB},
    {"rArr", 0x021D2},
    {"rAtail", 0x0291C},
    {"rBarr", 0x0290F},
    {"rHar", 0x02964},
    {"racute", 0x00155},
    {"radic", 0x0221A},
    {"raemptyv", 0x029B3},
    {"rang", 0x027E9},
    {"rangd", 0x02992},
    {"range", 0x029A5},
    {"rangle", 0x027E9},
    {"raquo", 0x000BB},
    {"rarr", 0x02192},
    {"rarrap", 0x02975},
    {"rarrb", 0x021E5},
    {"rarrbfs", 0x02920},
    {"rarrc", 0x02933},
    {"rarrfs", 0x0291E},
    {"rarrhk", 0x021AA},
    {"rarrlp", 0x021AC},
    {"rarrpl", 0x02945},
    {"rarrsim", 0x02974},
    {"rarrtl", 0x021A3},
    {"rarrw", 0x0219D},
    {"ratail", 0x0291A},
    {"ratio", 0x02236},
    {"rationals", 0x0211A},
    {"rbarr", 0x0290D},
    {"rbbrk", 0x02773},
    {"rbrace", 0x0007D},
    {"rbrack", 0x0005D},
    {"rbrke", 0x0298C},
    {"rbrksld", 0x0298E},
    {"rbrkslu", 0x02990},
    {"rcaron", 0x00159},
    {"rcedil", 0x00157},
    {"rceil", 0x02309},
    {"rcub", 0x0007D},
    {"rcy", 0x00440},
    {"rdca", 0x02937},
    {"rdldhar", 0x02969},
    {"rdquo", 0x0201D},
    {"rdquor", 0x0201D},
    {"rdsh", 0x021B3},
    {"real", 0x0211C},
    {"realine", 0x0211B},
    {"realpart", 0x0211C},
    {"reals", 0x0211D},
    {"rect", 0x025AD},
    {"reg", 0x000AE},
    {"rfisht", 0x0297D},
    {"rfloor", 0x0230B},
    {"rfr", 0x1D52F},
    {"rhard", 0x021C1},
    {"rharu", 0x021C0},
    {"rharul", 0x0296C},
    {"rho", 0x003C1},
    {"rhov", 0x003F1},
    {"rightarrow", 0x02192},
    {"rightarrowtail", 0x021A3},
    {"rightharpoondown", 0x021C1},
    {"rightharpoonup", 0x021C0},
    {"rightleftarrows", 0x021C4},
    {"rightleftharpoons", 0x021CC},
    {"rightrightarrows", 0x021C9},
    {"rightsquigarrow", 0x0219D},
    {"rightthreetimes", 0x022CC},
    {"ring", 0x002DA},
    {"risingdotseq", 0x02253},
    {"rlarr", 0x021C4},
    {"rlhar", 0x021CC},
    {"rlm", 0x0200F},
    {"rmoust", 0x023B1},
    {"rmoustache", 0x023B1},
    {"rnmid", 0x02AEE},
    {"roang", 0x027ED},
    {"roarr", 0x021FE},
    {"robrk", 0x027E7},
    {"ropar", 0x02986},
    {"ropf", 0x1D563},
    {"roplus", 0x02A2E},
    {"rotimes", 0x02A35},
    {"rpar", 0x00029},
    {"rpargt", 0x02994},
    {"rppolint", 0x02A12},
    {"rrarr", 0x021C9},
    {"rsaquo", 0x0203A},
    {"rscr", 0x1D4C7},
    {"rsh", 0x021B1},
    {"rsqb", 0x0005D},
    {"rsquo", 0x02019},
    {"rsquor", 0x02019},
    {"rthree", 0x022CC},
    {"rtimes", 0x022CA},
    {"rtri", 0x025B9},
    {"rtrie", 0x022B5},
    {"rtrif", 0x025B8},
    {"rtriltri", 0x029CE},
    {"ruluhar", 0x02968},
    {"rx", 0x0211E},
    {"sacute", 0x0015B},
    {"sbquo", 0x0201A},
    {"sc", 0x0227B},
    {"scE", 0x02AB4},
    {"scap", 0x02AB8},
    {"scaron", 0x00161},
    {"sccue", 0x0227D},
    {"sce", 0x02AB0},
    {"scedil", 0x0015F},
    {"scirc", 0x0015D},
    {"scnE", 0x02AB6},
    {"scnap", 0x02ABA},
    {"scnsim", 0x022E9},
    {"scpolint", 0x02A13},
    {"scsim", 0x0227F},
    {"scy", 0x00441},
    {"sdot", 0x022C5},
    {"sdotb", 0x022A1},
    {"sdote", 0x02A66},
    {"seArr", 0x021D8},
    {"searhk", 0x02925},
    {"searr", 0x02198},
    {"searrow", 0x02198},
    {"sect", 0x000A7},
    {"semi", 0x0003B},
    {"seswar", 0x02929},
    {"setminus", 0x02216},
    {"setmn", 0x02216},
    {"sext", 0x02736},
    {"sfr", 0x1D530},
    {"sfrown", 0x02322},
    {"sharp", 0x0266F},
    {"shchcy", 0x00449},
    {"shcy", 0x00448},
    {"shortmid", 0x02223},
    {"shortparallel", 0x02225},
    {"shy", 0x000AD},
    {"sigma", 0x003C3},
    {"sigmaf", 0x003C2},
    {"sigmav", 0x003C2},
    {"sim", 0x0223C},
    {"simdot", 0x02A6A},
    {"sime", 0x02243},
    {"simeq", 0x02243},
    {"simg", 0x02A9E},
    {"simgE", 0x02AA0},
    {"siml", 0x02A9D},
    {"simlE", 0x02A9F},
    {"simne", 0x02246},
    {"simplus", 0x02A24},
    {"simrarr", 0x02972},
    {"slarr", 0x02190},
    {"smallsetminus", 0x02216},
    {"smashp", 0x02A33},
    {"smeparsl", 0x029E4},
    {"smid", 0x02223},
    {"smile", 0x02323},
    {"smt", 0x02AAA},
    {"smte", 0x02AAC},
    {"softcy", 0x0044C},
    {"sol", 0x0002F},
    {"solb", 0x029C4},
    {"solbar", 0x0233F},
    {"sopf", 0x1D564},
    {"spades", 0x02660},
    {"spadesuit", 0x02660},
    {"spar", 0x02225},
    {"sqcap", 0x02293},
    {"sqcup", 0x02294},
    {"sqsub", 0x0228F},
    {"sqsube", 0x02291},
    {"sqsubset", 0x0228F},
    {"sqsubseteq", 0x02291},
    {"sqsup", 0x02290},
    {"sqsupe", 0x02292},
    {"sqsupset", 0x02290},
    {"sqsupseteq", 0x02292},
    {"squ", 0x025A1},
    {"square", 0x025A1},
    {"squarf", 0x025AA},
    {"squf", 0x025AA},
    {"srarr", 0x02192},
    {"sscr", 0x1D4C8},
    {"ssetmn", 0x02216},
    {"ssmile", 0x02323},
    {"sstarf", 0x022C6},
    {"star", 0x02606},
    {"starf", 0x02605},
    {"straightepsilon", 0x003F5},
    {"straightphi", 0x003D5},
    {"strns", 0x000AF},
    {"sub", 0x02282},
    {"subE", 0x02AC5},
    {"subdot", 0x02ABD},
    {"sube", 0x02286},
    {"subedot", 0x02AC3},
    {"submult", 0x02AC1},
    {"subnE", 0x02ACB},
    {"subne", 0x0228A},
    {"subplus", 0x02ABF},
    {"subrarr", 0x02979},
    {"subset", 0x02282},
    {"subseteq", 0x02286},
    {"subseteqq", 0x02AC5},
    {"subsetneq", 0x0228A},
    {"subsetneqq", 0x02ACB},
    {"subsim", 0x02AC7},
    {"subsub", 0x02AD5},
    {"subsup", 0x02AD3},
    {"succ", 0x0227B},
    {"succapprox", 0x02AB8},
    {"succcurlyeq", 0x0227D},
    {"succeq", 0x02AB0},
    {"succnapprox", 0x02ABA},
    {"succneqq", 0x02AB6},
    {"succnsim", 0x022E9},
    {"succsim", 0x0227F},
    {"sum", 0x02211},
    {"sung", 0x0266A},
    {"sup", 0x02283},
    {"sup1", 0x000B9},
    {"sup2", 0x000B2},
    {"sup3", 0x000B3},
    {"supE", 0x02AC6},
    {"supdot", 0x02ABE},
    {"supdsub", 0x02AD8},
    {"supe", 0x02287},
    {"supedot", 0x02AC4},
    {"suphsol", 0x027C9},
    {"suphsub", 0x02AD7},
    {"suplarr", 0x0297B},
    {"supmult", 0x02AC2},
    {"supnE", 0x02ACC},
    {"supne", 0x0228B},
    {"supplus", 0x02AC0},
    {"supset", 0x02283},
    {"supseteq", 0x02287},
    {"supseteqq", 0x02AC6},
    {"supsetneq", 0x0228B},
    {"supsetneqq", 0x02ACC},
    {"supsim", 0x02AC8},
    {"supsub", 0x02AD4},
    {"supsup", 0x02AD6},
    {"swArr", 0x021D9},
    {"swarhk", 0x02926},
    {"swarr", 0x02199},
    {"swarrow", 0x02199},
    {"swnwar", 0x0292A},
    {"szlig", 0x000DF},
    {"target", 0x02316},
    {"tau", 0x003C4},
    {"tbrk", 0x023B4},
    {"tcaron", 0x00165},
    {"tcedil", 0x00163},
    {"tcy", 0x00442},
    {"tdot", 0x020DB},
    {"telrec", 0x02315},
    {"tfr", 0x1D531},
    {"there4", 0x02234},
    {"therefore", 0x02234},
    {"theta", 0x003B8},
    {"thetasym", 0x003D1},
    {"thetav", 0x003D1},
    {"thickapprox", 0x02248},
    {"thicksim", 0x0223C},
    {"thinsp", 0x02009},
    {"thkap", 0x02248},
    {"thksim", 0x0223C},
    {"thorn", 0x000FE},
    {"tilde", 0x002DC},
    {"times", 0x000D7},
    {"timesb", 0x022A0},
    {"timesbar", 0x02A31},
    {"timesd", 0x02A30},
    {"tint", 0x0222D},
    {"toea", 0x02928},
    {"top", 0x022A4},
    {"topbot", 0x02336},
    {"topcir", 0x02AF1},
    {"topf", 0x1D565},
    {"topfork", 0x02ADA},
    {"tosa", 0x02929},
    {"tprime", 0x02034},
    {"trade", 0x02122},
    {"triangle", 0x025B5},
    {"triangledown", 0x025BF},
    {"triangleleft", 0x025C3},
    {"trianglelefteq", 0x022B4},
    {"triangleq", 0x0225C},
    {"triangleright", 0x025B9},
    {"trianglerighteq", 0x022B5},
    {"tridot", 0x025EC},
    {"trie", 0x0225C},
    {"triminus", 0x02A3A},
    {"triplus", 0x02A39},
    {"trisb", 0x029CD},
    {"tritime", 0x02A3B},
    {"trpezium", 0x023E2},
    {"tscr", 0x1D4C9},
    {"tscy", 0x00446},
    {"tshcy", 0x0045B},
    {"tstrok", 0x00167},
    {"twixt", 0x0226C},
    {"twoheadleftarrow", 0x0219E},
    {"twoheadrightarrow", 0x021A0},
    {"uArr", 0x021D1},
    {"uHar", 0x02963},
    {"uacute", 0x000FA},
    {"uarr", 0x02191},
    {"ubrcy", 0x0045E},
    {"ubreve", 0x0016D},
    {"ucirc", 0x000FB},
    {"ucy", 0x00443},
    {"udarr", 0x021C5},
    {"udblac", 0x00171},
    {"udhar", 0x0296E},
    {"ufisht", 0x0297E},
    {"ufr", 0x1D532},
    {"ugrave", 0x000F9},
    {"uharl", 0x021BF},
    {"uharr", 0x021BE},
    {"uhblk", 0x02580},
    {"ulcorn", 0x0231C},
    {"ulcorner", 0x0231C},
    {"ulcrop", 0x0230F},
    {"ultri", 0x025F8},
    {"umacr", 0x0016B},
    {"uml", 0x000A8},
    {"uogon", 0x00173},
    {"uopf", 0x1D566},
    {"uparrow", 0x02191},
    {"updownarrow", 0x02195},
    {"upharpoonleft", 0x021BF},
    {"upharpoonright", 0x021BE},
    {"uplus", 0x0228E},
    {"upsi", 0x003C5},
    {"upsih", 0x003D2},
    {"upsilon", 0x003C5},
    {"upuparrows", 0x021C8},
    {"urcorn", 0x0231D},
    {"urcorner", 0x0231D},
    {"urcrop", 0x0230E},
    {"uring", 0x0016F},
    {"urtri", 0x025F9},
    {"uscr", 0x1D4CA},
    {"utdot", 0x022F0},
    {"utilde", 0x00169},
    {"utri", 0x025B5},
    {"utrif", 0x025B4},
    {"uuarr", 0x021C8},
    {"uuml", 0x000FC},
    {"uwangle", 0x029A7},
    {"vArr", 0x021D5},
    {"vBar", 0x02AE8},
    {"vBarv", 0x02AE9},
    {"vDash", 0x022A8},
    {"vangrt", 0x0299C},
    {"varepsilon", 0x003F5},
    {"varkappa", 0x003F0},
    {"varnothing", 0x02205},
    {"varphi", 0x003D5},
    {"varpi", 0x003D6},
    {"varpropto", 0x0221D},
    {"varr", 0x02195},
    {"varrho", 0x003F1},
    {"varsigma", 0x003C2},
    {"vartheta", 0x003D1},
    {"vartriangleleft", 0x022B2},
    {"vartriangleright", 0x022B3},
    {"vcy", 0x00432},
    {"vdash", 0x022A2},
    {"vee", 0x02228},
    {"veebar", 0x022BB},
    {"veeeq", 0x0225A},
    {"vellip", 0x022EE},
    {"verbar", 0x0007C},
    {"vert", 0x0007C},
    {"vfr", 0x1D533},
    {"vltri", 0x022B2},
    {"vopf", 0x1D567},
    {"vprop", 0x0221D},
    {"vrtri", 0x022B3},
    {"vscr", 0x1D4CB},
    {"vzigzag", 0x0299A},
    {"wcirc", 0x00175},
    {"wedbar", 0x02A5F},
    {"wedge", 0x02227},
    {"wedgeq", 0x02259},
    {"weierp", 0x02118},
    {"wfr", 0x1D534},
    {"wopf", 0x1D568},
    {"wp", 0x02118},
    {"wr", 0x02240},
    {"wreath", 0x02240},
    {"wscr", 0x1D4CC},
    {"xcap", 0x022C2},
    {"xcirc", 0x025EF},
    {"xcup", 0x022C3},
    {"xdtri", 0x025BD},
    {"xfr", 0x1D535},
    {"xhArr", 0x027FA},
    {"xharr", 0x027F7},
    {"xi", 0x003BE},
    {"xlArr", 0x027F8},
    {"xlarr", 0x027F5},
    {"xmap", 0x027FC},
    {"xnis", 0x022FB},
    {"xodot", 0x02A00},
    {"xopf", 0x1D569},
    {"xoplus", 0x02A01},
    {"xotime", 0x02A02},
    {"xrArr", 0x027F9},
    {"xrarr", 0x027F6},
    {"xscr", 0x1D4CD},
    {"xsqcup", 0x02A06},
    {"xuplus", 0x02A04},
    {"xutri", 0x025B3},
    {"xvee", 0x022C1},
    {"xwedge", 0x022C0},
    {"yacute", 0x000FD},
    {"yacy", 0x0044F},
    {"ycirc", 0x00177},
    {"ycy", 0x0044B},
    {"yen", 0x000A5},
    {"yfr", 0x1D536},
    {"yicy", 0x00457},
    {"yopf", 0x1D56A},
    {"yscr", 0x1D4CE},
    {"yucy", 0x0044E},
    {"yuml", 0x000FF},
    {"zacute", 0x0017A},
    {"zcaron", 0x0017E},
    {"zcy", 0x00437},
    {"zdot", 0x0017C},
    {"zeetrf", 0x02128},
    {"zeta", 0x003B6},
    {"zfr", 0x1D537},
    {"zhcy", 0x00436},
    {"zigrarr", 0x021DD},
    {"zopf", 0x1D56B},
    {"zscr", 0x1D4CF},
    {"zwj", 0x0200D},
    {"zwnj", 0x0200C},
};
#define K_HTML_ENTITY_COUNT 2032


/* ---- character classes ---- */
static int h_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static char h_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}
static int h_isalnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}
/* Void elements: never take children; their start tag is complete. */
static const char* const k_void[] = {
    "area", "base", "basefont", "bgsound", "br", "col", "embed",
    "frame", "hr", "img", "input", "keygen", "link", "meta", "param",
    "source", "track", "wbr", NULL,
};
static int h_is_void(const char* name) {
    for (int i = 0; k_void[i]; i++)
        if (strcmp(name, k_void[i]) == 0) return 1;
    return 0;
}

/* Raw-text elements: content runs to the case-insensitive close
 * tag, no markup inside. script/style take entities verbatim. */
static int h_is_raw(const char* name) {
    return strcmp(name, "script") == 0 || strcmp(name, "style") == 0;
}

static int h_is_heading(const char* n) {
    return n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && n[2] == 0;
}

/* #659 WHATWG-only implied ends (12.2.6.4 "in body"): ruby
 * annotations (rb closes rb; rt/rp close rt/rb/rp — annotations
 * become siblings) plus rb/rt/rp/listing/plaintext closing an
 * open p. libxml2 keeps nesting; the html4 entry stays bare. */
static int h_closes_ww(const char* open, const char* start) {
    /* Heading starts pop a current heading (13.2.6.4.7 "in
     * body": <h1>x<h2> -> siblings). */
    if (h_is_heading(open) && h_is_heading(start)) return 1;
    int is_ruby_start = strcmp(start, "rb") == 0 ||
                        strcmp(start, "rt") == 0 ||
                        strcmp(start, "rp") == 0;
    if (is_ruby_start || strcmp(start, "listing") == 0 ||
        strcmp(start, "plaintext") == 0) {
        if (strcmp(open, "p") == 0) return 1;
        if (!is_ruby_start) return 0;
        if (strcmp(open, "rb") == 0) return 1;
        if (strcmp(start, "rb") == 0) return 0;
        if (strcmp(open, "rt") == 0 || strcmp(open, "rp") == 0)
            return 1;
    }
    return 0;
}

/* WHATWG: starts that close an open p in button scope
 * (13.2.6.4.7 "in body" block set + h/pre/form/li/dd/dt/plaintext/
 * hr/xmp). */
static int h_p_closes(const char* start) {
    static const char* const k[] = {
        "address", "article", "aside",  "blockquote", "center",
        "details", "dialog", "dir",     "div",        "dl",
        "fieldset", "figcaption", "figure", "footer", "header",
        "hgroup",  "main",    "menu",   "nav",        "ol",
        "p",       "search",  "section", "summary",   "ul",
        "h1",      "h2",      "h3",     "h4",         "h5",
        "h6",      "pre",     "listing", "form",      "li",
        "dd",      "dt",      "plaintext", "hr",     "xmp",
        NULL};
    for (int i = 0; k[i]; i++)
        if (strcmp(start, k[i]) == 0) return 1;
    return 0;
}

/* Implied-end sets: a start tag in `closes` closes any open
 * element named `name` (HTML4 §7.5.4 / table model). */
static const char* const k_p_closers[] = {
    "address", "article", "aside", "blockquote", "details", "dialog",
    "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer",
    "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup",
    "hr", "li", "main", "menu", "nav", "ol", "p", "pre", "section",
    "table", "ul", NULL,
};
static int h_in_list(const char* const* list, const char* name) {
    for (int i = 0; list[i]; i++)
        if (strcmp(list[i], name) == 0) return 1;
    return 0;
}
/* Does starting `start` close an open element named `open`? */
static int h_closes(const char* open, const char* start) {
    if (strcmp(open, "p") == 0 && h_in_list(k_p_closers, start))
        return 1;
    if (strcmp(open, "li") == 0 && strcmp(start, "li") == 0)
        return 1;
    if ((strcmp(open, "dt") == 0 || strcmp(open, "dd") == 0) &&
        (strcmp(start, "dt") == 0 || strcmp(start, "dd") == 0))
        return 1;
    if ((strcmp(open, "td") == 0 || strcmp(open, "th") == 0) &&
        (strcmp(start, "td") == 0 || strcmp(start, "th") == 0 ||
         strcmp(start, "tr") == 0 || strcmp(start, "tbody") == 0 ||
         strcmp(start, "thead") == 0 || strcmp(start, "tfoot") == 0 ||
         strcmp(start, "table") == 0))
        return 1;
    if (strcmp(open, "tr") == 0 &&
        (strcmp(start, "tr") == 0 || strcmp(start, "tbody") == 0 ||
         strcmp(start, "thead") == 0 || strcmp(start, "tfoot") == 0 ||
         strcmp(start, "table") == 0))
        return 1;
    if ((strcmp(open, "thead") == 0 || strcmp(open, "tbody") == 0 ||
         strcmp(open, "tfoot") == 0) &&
        (strcmp(start, "tbody") == 0 || strcmp(start, "tfoot") == 0 ||
         strcmp(start, "table") == 0))
        return 1;
    if (strcmp(open, "option") == 0 &&
        (strcmp(start, "option") == 0 || strcmp(start, "optgroup") == 0 ||
         strcmp(start, "select") == 0))
        return 1;
    if (strcmp(open, "optgroup") == 0 && strcmp(start, "optgroup") == 0)
        return 1;
    /* Same-name non-container nesting is never implied except the
     * cases above — a <div><div> nests. */
    return 0;
}

/* ---- HTML entity decode: named table + numeric, lenient ---- */
/* #848: the old lookup was a linear scan with a strlen per entry
 * — O(2032 x len) per entity reference, superlinear in entity
 * count. A lazily-sorted pointer index turns each lookup into a
 * binary search. */
typedef const HtmlEnt* HtmlEntPtr;
static HtmlEntPtr h_ent_index[K_HTML_ENTITY_COUNT];
static int h_ent_sorted = 0;

static int h_ent_cmp(const void* a, const void* b) {
    return strcmp((*(const HtmlEntPtr*)a)->name,
                  (*(const HtmlEntPtr*)b)->name);
}

static uint32_t h_entity_lookup(const char* name, size_t len) {
    if (!h_ent_sorted) {
        for (size_t i = 0; i < K_HTML_ENTITY_COUNT; i++)
            h_ent_index[i] = &k_html_entities[i];
        qsort(h_ent_index, K_HTML_ENTITY_COUNT,
              sizeof(h_ent_index[0]), h_ent_cmp);
        h_ent_sorted = 1;
    }
    size_t lo = 0, hi = K_HTML_ENTITY_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char* n = h_ent_index[mid]->name;
        int c = strncmp(n, name, len);
        if (c == 0 && n[len] != 0) c = 1;   /* entry longer than key */
        if (c == 0) return h_ent_index[mid]->cp;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}
/* Text-context decode (no attribute literal guard). */
static char* h_decode_ex(LeptrisMemoryPool* pool, const char* s,
                         const char* e, int in_attr, int whatwg);
/* WHATWG 13.2.5.84 numeric-reference end states: the C1 range
 * remaps through the Windows-1252 table (rows without an entry
 * stay as-is); everything else out of range, and surrogates and
 * NUL, become U+FFFD. */
static uint32_t h_numref_fix(long v) {
    static const struct { long from; uint32_t to; } k_c1[] = {
        {0x80, 0x20AC}, {0x82, 0x201A}, {0x83, 0x0192}, {0x84, 0x201E},
        {0x85, 0x2026}, {0x86, 0x2020}, {0x87, 0x2021}, {0x88, 0x02C6},
        {0x89, 0x2030}, {0x8A, 0x0160}, {0x8B, 0x2039}, {0x8C, 0x0152},
        {0x8E, 0x017D}, {0x91, 0x2018}, {0x92, 0x2019}, {0x93, 0x201C},
        {0x94, 0x201D}, {0x95, 0x2022}, {0x96, 0x2013}, {0x97, 0x2014},
        {0x98, 0x02DC}, {0x99, 0x2122}, {0x9A, 0x0161}, {0x9B, 0x203A},
        {0x9C, 0x0153}, {0x9E, 0x017E}, {0x9F, 0x0178},
    };
    if (v <= 0 || v > 0x10FFFF) return 0xFFFD;
    if (v >= 0xD800 && v <= 0xDFFF) return 0xFFFD;
    if (v >= 0x80 && v <= 0x9F)
        for (size_t i = 0; i < sizeof(k_c1) / sizeof(k_c1[0]); i++)
            if (k_c1[i].from == v) return k_c1[i].to;
    return (uint32_t)v;
}

static char* h_decode_ww(LeptrisMemoryPool* pool, const char* s,
                         const char* e, int in_attr, int whatwg) {
    return h_decode_ex(pool, s, e, in_attr, whatwg);
}
static size_t h_utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode entities in [s, e) into a pooled NUL-terminated copy.
 * Unknown references pass through verbatim (HTML is lenient). */
/* WHATWG 12.2.5.73: the legacy HTML4 references that are
 * valid WITHOUT the trailing ';'. */
static const char* const k_legacy_ent[] = {
    "AElig", "AMP",   "Aacute", "Acirc",  "Agrave", "Atilde",
    "Auml",  "COPY",  "Ccedil", "ETH",    "Eacute", "Ecirc",
    "Egrave", "Euml", "GT",     "Iacute", "Icirc",  "Igrave",
    "Iuml",  "Ntilde", "Oacute", "Ocirc", "Ograve", "Oslash",
    "Otilde", "Ouml", "QUOT",   "REG",    "THORN",  "Uacute",
    "Ucirc", "Ugrave", "Uuml",  "Yacute", "aacute", "acirc",
    "acute", "aelig", "agrave", "amp",    "aring",  "atilde",
    "auml",  "brvbar", "ccedil", "cedil", "cent",   "copy",
    "curren", "deg",  "divide", "eacute", "ecirc",  "egrave",
    "eth",   "euml",  "frac12", "frac14", "frac34", "gt",
    "iacute", "icirc", "iexcl", "igrave", "iquest", "iuml",
    "laquo", "lt",    "macr",   "micro",  "middot", "nbsp",
    "not",   "ntilde", "oacute", "ocirc", "ograve", "ordf",
    "ordm",  "oslash", "otilde", "ouml",  "para",   "plusmn",
    "pound", "quot",  "raquo",  "reg",    "sect",   "shy",
    "sup1",  "sup2",  "sup3",   "szlig",  "thorn",  "times",
    "uacute", "ucirc", "ugrave", "uml",   "uuml",   "yacute",
    "yen",   "yuml",  NULL};
static int h_is_legacy_ent(const char* s, size_t n) {
    for (int i = 0; k_legacy_ent[i]; i++) {
        const char* t = k_legacy_ent[i];
        size_t tl = strlen(t);
        if (tl == n) {
            size_t j = 0;
            for (; j < n; j++)
                if (s[j] != t[j]) break;
            if (j == n) return 1;
        }
    }
    return 0;
}

static char* h_decode_ex(LeptrisMemoryPool* pool, const char* s,
                         const char* e, int in_attr, int whatwg) {
    size_t cap = (size_t)(e - s) + 8;
    char* out = (char*)leptris_pool_alloc(pool, cap);
    if (!out) return NULL;
    size_t len = 0;
    while (s < e) {
        if (*s == '&') {
            uint32_t cp = 0;
            const char* adv = NULL;
            /* WHATWG 12.2.5.78: numeric references decode with
             * or without the ';' (html4/libxml2 mode keeps the
             * strict ';' form). */
            if (s + 1 < e && s[1] == '#') {
                const char* q = s + 2;
                int hex = 0;
                if (q < e && (*q == 'x' || *q == 'X')) {
                    hex = 1;
                    q++;
                }
                const char* ds = q;
                while (q < e &&
                       (hex ? ((*q >= '0' && *q <= '9') ||
                               (*q >= 'a' && *q <= 'f') ||
                               (*q >= 'A' && *q <= 'F'))
                            : (*q >= '0' && *q <= '9')))
                    q++;
                if (q > ds) {
                    char buf[16];
                    size_t dn = (size_t)(q - ds);
                    if (dn > sizeof(buf) - 1) dn = sizeof(buf) - 1;
                    memcpy(buf, ds, dn);
                    buf[dn] = 0;
                    long v = strtol(buf, NULL, hex ? 16 : 10);
                    if (whatwg) {
                        /* 13.2.5.84 end states: overflow saturates
                         * at LONG_MAX (strtol) and lands in the
                         * FFFD bucket with everything else; the
                         * missing semicolon is tolerated. */
                        cp = h_numref_fix(v);
                        adv = q;
                        if (adv < e && *adv == ';') adv++;
                    } else {
                        int ok = v > 0 && v <= 0x10FFFF;
                        ok = ok && q < e && *q == ';';
                        if (ok) {
                            cp = (uint32_t)v;
                            adv = q;
                            if (adv < e && *adv == ';') adv++;
                        }
                    }
                }
            } else {
                /* WHATWG 12.2.5.73: longest-prefix named match.
                 * With the ';' any table name matches; without it
                 * only the legacy subset — and in attributes not
                 * when '=' or an alphanumeric follows. */
                const char* sc = s + 1;
                size_t probe = 0;
                while (sc < e && *sc != ';' && probe < 40 &&
                       h_isalnum(*sc)) {
                    sc++;
                    probe++;
                }
                int semi = (sc < e && *sc == ';');
                if (!whatwg) {
                    /* html4/libxml2 compat: ';' required, whole
                     * run exact. */
                    if (semi)
                        cp = h_entity_lookup(s + 1, probe);
                    if (cp) adv = sc + 1;
                }
                for (size_t tl = probe; tl > 0 && !cp; tl--) {
                    if (tl == probe && semi) {
                        cp = h_entity_lookup(s + 1, tl);
                        if (cp) adv = sc + 1;
                    }
                    if (whatwg && !cp && !(tl == probe && semi) &&
                        h_is_legacy_ent(s + 1, tl)) {
                        const char* nx = s + 1 + tl;
                        if (!in_attr ||
                            !(nx < e &&
                              (*nx == '=' || h_isalnum(*nx)))) {
                            cp = h_entity_lookup(s + 1, tl);
                            if (cp) adv = nx;
                        }
                    }
                }
            }
            if (cp && adv) {
                if (len + 4 >= cap) { /* bounded: probe <= 40 bytes */ }
                len += h_utf8_encode(cp, out + len);
                s = adv;
                continue;
            }
        }
        if (len + 1 >= cap) { out = NULL; return NULL; }
        out[len++] = *s++;
    }
    out[len] = 0;
    return out;
}

/* ---- builder ---- */
typedef struct {
    struct leptris_document* doc;
    LeptrisMemoryPool* pool;
    LeptrisElement open[256];
    /* #659 foreign content: per-slot namespace of the open stack
     * (0 HTML, 1 SVG, 2 MathML) — parallel to open[]. */
    uint8_t open_ns[256];
    size_t depth;
    LeptrisElement root;        /* first top-level element */
    LeptrisNodeRef top_head;    /* top-level chain: text, comments, root */
    LeptrisNodeRef top_tail;
    /* #659 WHATWG initial mode: comments tokenized while the
     * document is still in its initial phase (no start/end tag,
     * no non-whitespace text yet) are children of the DOCUMENT —
     * kept ahead of the tree at commit (html4 keeps the libxml2
     * shape). */
    int left_initial;
    LeptrisNodeRef prolog_head, prolog_tail;
    /* #659 master mode flag (the per-slice flags below derive from
     * the same html_parse_shared arg). */
    int whatwg;
    /* #659 frameset mode (WHATWG): a <frameset> before any body
     * content REPLACES the body — the commit synthesis emits
     * html > [head, frameset]. */
    int frameset;
    /* #659: structural <head>/<body> tags are dropped but their
     * ATTRIBUTES land on the synthesized elements (name/value
     * pool-string pairs). */
    char* head_attrs[32];
    int head_attr_n;
    char* body_attrs[32];
    int body_attr_n;
    char* html_attrs[32];
    int html_attr_n;
    /* #659 two-mode split: leptris_parse_html_string is the WHATWG
     * engine (full "in head" set: script/style/noscript/template
     * ... lift into the implied head); the new
     * leptris_parse_html4_string keeps the libxml2/Nokogiri compat
     * shape (title/meta/link/base only — libxml2 leaves leading
     * script/style in body). */
    int whatwg_head_set;
    /* #659: a structural <body> was seen (lift_closed) — the
     * head-lift run may not extend past lift_boundary, the last
     * node appended before it (NULL = nothing was). */
    int lift_closed;
    LeptrisNodeRef lift_boundary;
    /* #659 foster parenting: WHATWG 12.2.6.1 — text (and non-table
     * elements) arriving with a table-context insertion point go
     * BEFORE the table in its parent. libxml2 keeps them in the
     * table; the html4 entry does not foster. Same mode arg. */
    int whatwg_foster;
    /* #659 adoption agency (simplified 8.2.5.4): formatting
     * elements open ABOVE a matched close are cloned and reopened
     * at the new insertion point, so misnested content keeps its
     * formatting scope (<b>1<i>2</b>3</i> -> <b>1<i>2</i></b><i>3
     * </i>). libxml2 pops them away; html4 keeps that shape. */
    int whatwg_adopt;
    /* #659 WHATWG list of active formatting elements (13.2.4.3):
     * each entry is the element created for its token (clones are
     * re-created from it); markers (el == NULL, afe_marker == 1)
     * fence applet/object/marquee/td/th/caption scopes so
     * formatting cannot leak in or out. */
    LeptrisElement afe[64];
    unsigned char afe_marker[64];
    int afe_n;
    /* #659 per-template insertion mode, indexed by the template's
     * open-stack index (see h_tmpl_content_start). */
    unsigned char tmpl_mode[256];
} HBuilder;

/* Pool a NUL-terminated ASCII-lowercased copy of [s, s+len). */
static char* h_pooled_lower(LeptrisMemoryPool* pool, const char* s,
                            size_t len) {
    char* out = (char*)leptris_pool_alloc(pool, len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) out[i] = h_lower(s[i]);
    out[len] = 0;
    return out;
}

static void h_top_append(HBuilder* b, LeptrisNodeRef n) {
    if (!b->top_tail) b->top_head = n;
    else leptris_node_set_next_sibling(b->top_tail, n);
    b->top_tail = n;
}

static int h_ieq_raw(const char* a, const char* bname);

/* #659: is the top chain still nothing but head-liftable
 * content (so a <frameset> may still replace the body)? */
static int h_body_still_empty(HBuilder* b) {
    for (LeptrisNodeRef n = b->top_head; n;
         n = leptris_node_get_next_sibling(n)) {
        int ty = leptris_node_get_type(n);
        if (ty == LEPTRIS_NODE_TYPE_ELEMENT) {
            const char* nm = leptris_element_name((LeptrisElement)n);
            if (!(h_ieq_raw(nm, "title") || h_ieq_raw(nm, "meta") ||
                  h_ieq_raw(nm, "link") || h_ieq_raw(nm, "base") ||
                  h_ieq_raw(nm, "basefont") ||
                  h_ieq_raw(nm, "bgsound") ||
                  h_ieq_raw(nm, "script") || h_ieq_raw(nm, "style") ||
                  h_ieq_raw(nm, "noscript") ||
                  h_ieq_raw(nm, "noframes") ||
                  h_ieq_raw(nm, "template")))
                return 0;
        } else if (ty == LEPTRIS_NODE_TYPE_TEXT) {
            const char* t = leptris_text_node_get_content(n);
            if (t)
                for (const char* p = t; *p; p++)
                    if (*p != ' ' && *p != '\t' && *p != '\n' &&
                        *p != '\r')
                        return 0;
        }
        /* comments/PIs are neutral */
    }
    return 1;
}

/* #659: <noscript> opened in the head phase (scripting off) —
 * WHATWG 12.2.6.4.5 "in head noscript": head content and
 * comments stay inside; the first body-ish token pops it. */
static int h_in_head_noscript(HBuilder* b) {
    return b->whatwg && b->depth == 1 && !b->lift_closed &&
           h_ieq_raw(leptris_element_name(b->open[0]), "noscript") &&
           h_body_still_empty(b);
}

/* #659: stash the attributes of a dropped structural tag —
 * pool-owned flat name/value pairs, applied to the synthesized
 * element at commit. */
static void h_stash_attrs(HBuilder* b, const char* q, const char* end,
                          char** attrs, int* n) {
    while (q < end && *q != '>' && *n + 1 < 32) {
        while (q < end && h_is_ws(*q)) q++;
        if (q >= end || *q == '>') break;
        if (*q == '/') {
            q++;
            continue;
        }
        const char* as = q;
        while (q < end && !h_is_ws(*q) && *q != '=' && *q != '>' &&
               *q != '/')
            q++;
        size_t alen = (size_t)(q - as);
        if (!alen) {
            q++;
            continue;
        }
        char* aname = h_pooled_lower(b->pool, as, alen);
        const char* vs = NULL;
        size_t vlen = 0;
        const char* scan = q;
        while (scan < end && h_is_ws(*scan)) scan++;
        if (scan < end && *scan == '=') {
            scan++;
            while (scan < end && h_is_ws(*scan)) scan++;
            if (scan < end && (*scan == '\'' || *scan == '"')) {
                char quote = *scan++;
                vs = scan;
                while (scan < end && *scan != quote) scan++;
                vlen = (size_t)(scan - vs);
                if (scan < end) scan++;
            } else {
                vs = scan;
                while (scan < end && !h_is_ws(*scan) && *scan != '>')
                    scan++;
                vlen = (size_t)(scan - vs);
            }
            q = scan;
        }
        char* aval = vs ? h_decode_ex(b->pool, vs, vs + vlen, 1,
                                      b->whatwg)
                        : (char*)"";
        if (aname && aval) {
            attrs[(*n)++] = aname;
            attrs[(*n)++] = aval;
        }
    }
}

/* Apply stashed structural attributes to a synthesized element. */
static void h_apply_attrs(HBuilder* b, LeptrisElement e, char** attrs,
                          int n) {
    for (int i = 0; i + 1 < n; i += 2)
        leptris_element_add_attribute(
            e, leptris_sv_from_cstr(attrs[i]),
            leptris_sv_from_cstr(attrs[i + 1]), b->pool);
}


/* WHATWG 12.2.6.1: only nodes NOT allowed in table context foster
 * — table-structure elements stay, whitespace-only text stays in
 * the table ("in table text"), comments stay. */
static int h_fosterable(HBuilder* b, LeptrisNodeRef n) {
    int ty = leptris_node_get_type(n);
    if (ty == LEPTRIS_NODE_TYPE_COMMENT) return 0;
    if (ty == LEPTRIS_NODE_TYPE_TEXT) {
        const char* t = leptris_text_get_content((LeptrisTextNode*)n);
        if (!t) return 0;
        for (const char* p = t; *p; p++)
            if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                return 1;
        return 0;   /* whitespace-only stays */
    }
    if (ty != LEPTRIS_NODE_TYPE_ELEMENT) return 0;
    const char* nname = leptris_element_name((LeptrisElement)n);
    return !(h_ieq_raw(nname, "table") || h_ieq_raw(nname, "tbody") ||
             h_ieq_raw(nname, "thead") || h_ieq_raw(nname, "tfoot") ||
             h_ieq_raw(nname, "tr") || h_ieq_raw(nname, "td") ||
             h_ieq_raw(nname, "th") || h_ieq_raw(nname, "caption") ||
             h_ieq_raw(nname, "col") || h_ieq_raw(nname, "colgroup") ||
             h_ieq_raw(nname, "tbody") || h_ieq_raw(nname, "form") ||
             h_ieq_raw(nname, "script") || h_ieq_raw(nname, "style") ||
             h_ieq_raw(nname, "template") || h_ieq_raw(nname, "input"));
}

/* WHATWG formatting elements (the adoption agency's subject). */
static int h_is_formatting(const char* n) {
    return h_ieq_raw(n, "a") || h_ieq_raw(n, "b") ||
           h_ieq_raw(n, "big") || h_ieq_raw(n, "code") ||
           h_ieq_raw(n, "em") || h_ieq_raw(n, "font") ||
           h_ieq_raw(n, "i") || h_ieq_raw(n, "nobr") ||
           h_ieq_raw(n, "s") || h_ieq_raw(n, "small") ||
           h_ieq_raw(n, "strike") || h_ieq_raw(n, "strong") ||
           h_ieq_raw(n, "tt") || h_ieq_raw(n, "u");
}

/* WHATWG "special" category (13.2.4.2) — the adoption agency's
 * furthest-block candidates. */
static int h_is_special_ww(const char* n) {
    static const char* const k[] = {
        "address",  "applet",  "area",   "article",  "aside",
        "base",     "basefont", "bgsound", "blockquote", "body",
        "br",       "button",  "caption", "center",  "col",
        "colgroup", "dd",      "details", "dir",     "div",
        "dl",       "dt",      "embed",  "fieldset", "figcaption",
        "figure",   "footer",  "form",   "frame",    "frameset",
        "h1",       "h2",      "h3",     "h4",       "h5",
        "h6",       "head",    "header", "hgroup",   "hr",
        "html",     "iframe",  "img",    "input",    "keygen",
        "li",       "link",    "listing", "main",    "marquee",
        "menu",     "meta",    "nav",    "noembed",  "noframes",
        "noscript", "object",  "ol",     "p",        "param",
        "plaintext", "pre",    "script", "search",   "section",
        "select",   "source",  "style",  "summary",  "table",
        "tbody",    "td",      "template", "textarea", "tfoot",
        "th",       "thead",   "title",  "tr",       "track",
        "ul",       "wbr",     "xmp",    NULL};
    if (!n) return 0;
    for (int i = 0; k[i]; i++)
        if (strcmp(n, k[i]) == 0) return 1;
    return 0;
}

#define H_NS_HTML 0
#define H_NS_SVG 1
#define H_NS_MATH 2

/* Foreign integration points (13.2.6 tree construction
 * dispatcher): MathML text integration points (mi/mo/mn/ms/
 * mtext) and HTML integration points (annotation-xml with an
 * HTML encoding, svg foreignObject/desc/title). They terminate
 * every scope walk (13.2.4.2 "in scope" list) and the foreign
 * breakout pop. */
static int h_is_int_point(HBuilder* b, size_t idx) {
    if (b->open_ns[idx] == H_NS_HTML) return 0;
    const char* tn = leptris_element_name(b->open[idx]);
    if (!tn) return 0;
    if (h_ieq_raw(tn, "mi") || h_ieq_raw(tn, "mo") ||
        h_ieq_raw(tn, "mn") || h_ieq_raw(tn, "ms") ||
        h_ieq_raw(tn, "mtext") || h_ieq_raw(tn, "foreignobject") ||
        h_ieq_raw(tn, "desc") || h_ieq_raw(tn, "title"))
        return 1;
    if (h_ieq_raw(tn, "annotation-xml")) {
        for (struct leptris_attribute* a =
                 leptris_element_get_first_attribute(b->open[idx]);
             a; a = leptris_attr_next(a)) {
            const char* cn = attr_cname(a);
            if (cn && strcmp(cn, "encoding") == 0) {
                const char* v = attr_cvalue(a);
                if (h_ieq_raw(v, "text/html") ||
                    h_ieq_raw(v, "application/xhtml+xml"))
                    return 1;
            }
        }
    }
    return 0;
}

/* Start tags that DO reconstruct the active formatting list
 * before inserting (13.2.6.4.7): everything except the structural
 * head set, the block-level set (they close p instead), the table
 * family, and raw-text elements. */
static int h_reconstructs(const char* n) {
    static const char* const no[] = {
        "html",  "head",   "body",   "frameset", "base",  "basefont",
        "bgsound", "link", "meta",   "script",   "style", "template",
        "title", "noframes", "address", "article", "aside",
        "blockquote", "center", "details", "dialog", "dir",
        "div",   "dl",     "fieldset", "figcaption", "figure",
        "footer", "header", "hgroup", "main",    "menu",  "nav",
        "ol",    "p",      "search", "section",  "summary", "ul",
        "h1",    "h2",     "h3",     "h4",       "h5",    "h6",
        "pre",   "listing", "form",  "li",       "dd",    "dt",
        "plaintext", "textarea", "table", "hr",  "caption", "col",
        "colgroup", "frame", "tbody", "tfoot",   "thead", "td",
        "th",    "tr",     NULL};
    for (int i = 0; no[i]; i++)
        if (strcmp(n, no[i]) == 0) return 0;
    return 1;
}

/* ---- #659 foreign content (WHATWG 12.2.6.5) ---- */

static const char H_SVG_URI[] = "http://www.w3.org/2000/svg";
static const char H_MATH_URI[] = "http://www.w3.org/1998/Math/MathML";

/* HTML breakout tags: a start tag with one of these names (font
 * only with color/face/size — checked by the caller) pops the
 * foreign scope and is reprocessed under HTML rules. */
static int h_is_breakout(const char* n) {
    static const char* const k[] = {
        "b",      "big",   "blockquote", "body", "br",    "center",
        "code",   "dd",    "div",        "dl",   "dt",    "em",
        "embed",  "h1",    "h2",         "h3",   "h4",    "h5",
        "h6",     "head",  "hr",         "i",    "img",   "li",
        "listing", "menu", "meta",       "nobr", "ol",    "p",
        "pre",    "ruby",  "s",          "small", "span", "strong",
        "strike", "sub",   "sup",        "table", "tt",   "u",
        "ul",     "var",   NULL};
    for (int i = 0; k[i]; i++)
        if (strcmp(n, k[i]) == 0) return 1;
    return 0;
}

/* SVG element-name adjustment (lowercased source -> camelCase). */
static const char* h_svg_name(const char* n) {
    static const struct {
        const char *lo, *adj;
    } k[] = {
        {"altglyph", "altGlyph"},
        {"altglyphdef", "altGlyphDef"},
        {"altglyphitem", "altGlyphItem"},
        {"animatecolor", "animateColor"},
        {"animatemotion", "animateMotion"},
        {"animatetransform", "animateTransform"},
        {"clippath", "clipPath"},
        {"feblend", "feBlend"},
        {"fecolormatrix", "feColorMatrix"},
        {"fecomponenttransfer", "feComponentTransfer"},
        {"fecomposite", "feComposite"},
        {"feconvolvematrix", "feConvolveMatrix"},
        {"fediffuselighting", "feDiffuseLighting"},
        {"fedisplacementmap", "feDisplacementMap"},
        {"fedistantlight", "feDistantLight"},
        {"fedropshadow", "feDropShadow"},
        {"feflood", "feFlood"},
        {"fefunca", "feFuncA"},
        {"fefuncb", "feFuncB"},
        {"fefuncg", "feFuncG"},
        {"fefuncr", "feFuncR"},
        {"fegaussianblur", "feGaussianBlur"},
        {"feimage", "feImage"},
        {"femerge", "feMerge"},
        {"femergenode", "feMergeNode"},
        {"femorphology", "feMorphology"},
        {"feoffset", "feOffset"},
        {"fepointlight", "fePointLight"},
        {"fespecularlighting", "feSpecularLighting"},
        {"fespotlight", "feSpotLight"},
        {"fetile", "feTile"},
        {"feturbulence", "feTurbulence"},
        {"foreignobject", "foreignObject"},
        {"glyphref", "glyphRef"},
        {"lineargradient", "linearGradient"},
        {"radialgradient", "radialGradient"},
        {"textpath", "textPath"},
        {NULL, NULL}};
    for (int i = 0; k[i].lo; i++)
        if (strcmp(n, k[i].lo) == 0) return k[i].adj;
    return NULL;
}

/* Attribute-name adjustment (MathML definitionURL + the SVG
 * table); NULL = keep the lowercased source name. */
static const char* h_attr_name(int ns, const char* n) {
    static const struct {
        const char *lo, *adj;
    } k[] = {
        {"attributename", "attributeName"},
        {"attribution", "attributeType"},
        {"basefrequency", "baseFrequency"},
        {"baseprofile", "baseProfile"},
        {"calcmode", "calcMode"},
        {"clippathunits", "clipPathUnits"},
        {"diffuseconstant", "diffuseConstant"},
        {"edgemode", "edgeMode"},
        {"filterunits", "filterUnits"},
        {"glyphref", "glyphRef"},
        {"gradienttransform", "gradientTransform"},
        {"gradientunits", "gradientUnits"},
        {"kernelmatrix", "kernelMatrix"},
        {"kernelunitlength", "kernelUnitLength"},
        {"keypoints", "keyPoints"},
        {"keysplines", "keySplines"},
        {"keytimes", "keyTimes"},
        {"lengthadjust", "lengthAdjust"},
        {"limitingconeangle", "limitingConeAngle"},
        {"markerheight", "markerHeight"},
        {"markerunits", "markerUnits"},
        {"markerwidth", "markerWidth"},
        {"maskcontentunits", "maskContentUnits"},
        {"maskunits", "maskUnits"},
        {"numoctaves", "numOctaves"},
        {"pathlength", "pathLength"},
        {"patterncontentunits", "patternContentUnits"},
        {"patterntransform", "patternTransform"},
        {"patternunits", "patternUnits"},
        {"pointsatx", "pointsAtX"},
        {"pointsaty", "pointsAtY"},
        {"pointsatz", "pointsAtZ"},
        {"preservealpha", "preserveAlpha"},
        {"preserveaspectratio", "preserveAspectRatio"},
        {"primitiveunits", "primitiveUnits"},
        {"refx", "refX"},
        {"refy", "refY"},
        {"repeatcount", "repeatCount"},
        {"repeatdur", "repeatDur"},
        {"requiredextensions", "requiredExtensions"},
        {"requiredfeatures", "requiredFeatures"},
        {"specularconstant", "specularConstant"},
        {"specularexponent", "specularExponent"},
        {"spreadmethod", "spreadMethod"},
        {"startoffset", "startOffset"},
        {"stddeviation", "stdDeviation"},
        {"stitchtiles", "stitchTiles"},
        {"surfacescale", "surfaceScale"},
        {"systemlanguage", "systemLanguage"},
        {"tablevalues", "tableValues"},
        {"targetx", "targetX"},
        {"targety", "targetY"},
        {"textlength", "textLength"},
        {"viewbox", "viewBox"},
        {"viewtarget", "viewTarget"},
        {"xchannelselector", "xChannelSelector"},
        {"ychannelselector", "yChannelSelector"},
        {"zoomandpan", "zoomAndPan"},
        {NULL, NULL}};
    if (ns == H_NS_MATH)
        return strcmp(n, "definitionurl") == 0 ? "definitionURL" : NULL;
    if (ns != H_NS_SVG) return NULL;
    for (int i = 0; k[i].lo; i++)
        if (strcmp(n, k[i].lo) == 0) return k[i].adj;
    return NULL;
}

/* Namespace a start tag with this name lands in, given the open
 * stack: integration points resume HTML rules (12.2.6.5); math
 * text-integration keeps mglyph/malignmark MathML. */
static int h_start_ns(HBuilder* b, const char* name) {
    if (b->depth == 0) return H_NS_HTML;
    int top = b->open_ns[b->depth - 1];
    if (top == H_NS_HTML) return H_NS_HTML;
    const char* tn = leptris_element_name(b->open[b->depth - 1]);
    if (top == H_NS_MATH) {
        if (h_ieq_raw(tn, "mi") || h_ieq_raw(tn, "mo") ||
            h_ieq_raw(tn, "mn") || h_ieq_raw(tn, "ms") ||
            h_ieq_raw(tn, "mtext")) {
            if (strcmp(name, "mglyph") == 0 ||
                strcmp(name, "malignmark") == 0)
                return H_NS_MATH;
            return H_NS_HTML;   /* MathML text integration point */
        }
        if (h_ieq_raw(tn, "annotation-xml")) {
            /* HTML integration point iff encoding=text/html or
             * application/xhtml+xml (case-insensitive). */
            for (struct leptris_attribute* a =
                     leptris_element_get_first_attribute(
                         b->open[b->depth - 1]);
                 a; a = leptris_attr_next(a)) {
                const char* cn = attr_cname(a);
                if (cn && strcmp(cn, "encoding") == 0) {
                    const char* v = attr_cvalue(a);
                    if (h_ieq_raw(v, "text/html") ||
                        h_ieq_raw(v, "application/xhtml+xml"))
                        return H_NS_HTML;
                }
            }
            return H_NS_MATH;
        }
        return H_NS_MATH;
    }
    /* h_ieq_raw's second arg is lowercase by convention — the
     * stored SVG name is camelCase (foreignObject). */
    if (h_ieq_raw(tn, "foreignobject") || h_ieq_raw(tn, "desc") ||
        h_ieq_raw(tn, "title"))
        return H_NS_HTML;   /* SVG HTML integration points */
    return H_NS_SVG;
}

/* Namespace of the CURRENT insertion point (text/CDATA routing):
 * foreign iff the top is foreign and not an integration point. */
static int h_cur_ns(HBuilder* b) {
    return h_start_ns(b, "");
}

/* An open <select> swallows svg/math start tags (in-select mode
 * ignores unknown start tags; the html4 entry keeps libxml2's
 * keep-everything shape). */
static int h_in_select(HBuilder* b) {
    for (size_t i = b->depth; i > 0; i--) {
        const char* on = leptris_element_name(b->open[i - 1]);
        if (on && strcmp(on, "select") == 0) return 1;
    }
    return 0;
}

/* #659: index of the nearest open <template> on the stack, or
 * -1. The in-template insertion rules fence on it. */
static int h_template_idx(HBuilder* b) {
    for (size_t i = b->depth; i > 0; i--) {
        const char* n = leptris_element_name(b->open[i - 1]);
        if (n && h_ieq_raw(n, "template")) return (int)(i - 1);
    }
    return -1;
}

/* #659 per-template insertion modes (13.2.6.4.10): the saved mode a
 * template re-enters at content level, indexed by its open-stack
 * index. Drives wrap/drop for table-context start tokens arriving
 * with the template on top of the stack. The transitions mirror
 * the WHATWG reprocess chains (verified against gumbo's
 * handle_in_template/in_table_body/in_row): fresh templates open
 * rows/cells/sections BARE; after an explicit row closes (mode
 * in-table-body) a cell gets an implied tr; after a section closes
 * (mode in-table) a row gets its implied tbody; rows/sections with
 * nothing in table scope DROP (stray tokens in row/body context). */
#define H_TPLM_TEMPLATE 0u
#define H_TPLM_IN_TABLE 1u
#define H_TPLM_IN_TBODY 2u
#define H_TPLM_IN_ROW   3u
#define H_TPLM_IN_CGROUP 4u
#define H_TPLM_IN_BODY  5u

/* Actions for a table-context start token at template content.
 * h_tmpl_content_start advances the template's mode and returns:
 *  0 = open bare (no wrapper synthesis)
 *  1 = DROP the token entirely
 *  2 = open an implied tr first
 *  3 = open an implied tbody first
 *  4 = open implied tbody + tr
 *  5 = not template-governed (ordinary open) */
static int h_tmpl_content_start(HBuilder* b, const char* name) {
    int ti = (int)b->depth - 1; /* caller checked: top is template */
    if (strcmp(name, "template") == 0) return 5; /* head rules own it */
    int is_row = strcmp(name, "tr") == 0;
    int is_cell = strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
    int is_col = strcmp(name, "col") == 0;
    int is_group = strcmp(name, "caption") == 0 ||
                   strcmp(name, "colgroup") == 0 ||
                   strcmp(name, "tbody") == 0 ||
                   strcmp(name, "thead") == 0 ||
                   strcmp(name, "tfoot") == 0;
    /* 13.2.6.4.10 anything-else -> in-body: frame start tags are
     * ignored outright there, and frameset tokens vanish inside a
     * template (html5lib template.dat:41/67/93). */
    if (strcmp(name, "frame") == 0 || strcmp(name, "frameset") == 0)
        return 1;
    if (!is_row && !is_cell && !is_col && !is_group) {
        if (b->tmpl_mode[ti] == H_TPLM_TEMPLATE) {
            /* 13.2.6.4.10: head-family tokens (base/basefont/bgsound/
             * link/meta/noframes/script/style/title) process via
             * "in head" rules and leave the mode untouched; any
             * other start tag pushes in-body. */
            if (strcmp(name, "base") != 0 &&
                strcmp(name, "basefont") != 0 &&
                strcmp(name, "bgsound") != 0 &&
                strcmp(name, "link") != 0 &&
                strcmp(name, "meta") != 0 &&
                strcmp(name, "noframes") != 0 &&
                strcmp(name, "script") != 0 &&
                strcmp(name, "style") != 0 &&
                strcmp(name, "title") != 0)
                b->tmpl_mode[ti] = H_TPLM_IN_BODY;
        }
        /* in-column-group: non-table tokens are ignored too
         * (gumbo handle_in_column_group; template.dat:74). */
        if (b->tmpl_mode[ti] == H_TPLM_IN_CGROUP) return 1;
        return 5;
    }
    unsigned char m = b->tmpl_mode[ti];
    switch (m) {
    case H_TPLM_TEMPLATE:
        if (is_group) b->tmpl_mode[ti] = H_TPLM_IN_TABLE;
        else if (is_col) b->tmpl_mode[ti] = H_TPLM_IN_CGROUP;
        else if (is_row) b->tmpl_mode[ti] = H_TPLM_IN_TBODY;
        else b->tmpl_mode[ti] = H_TPLM_IN_ROW;
        return 0;
    case H_TPLM_IN_TABLE:
        if (is_row) { b->tmpl_mode[ti] = H_TPLM_IN_TBODY; return 3; }
        if (is_cell) { b->tmpl_mode[ti] = H_TPLM_IN_ROW; return 4; }
        if (is_col) b->tmpl_mode[ti] = H_TPLM_IN_CGROUP;
        return 0;
    case H_TPLM_IN_TBODY:
        if (is_cell) { b->tmpl_mode[ti] = H_TPLM_IN_ROW; return 2; }
        if (is_group) return 1; /* no open section in table scope */
        if (is_col) { b->tmpl_mode[ti] = H_TPLM_IN_CGROUP; return 0; }
        return 0; /* another row, bare */
    case H_TPLM_IN_ROW:
        if (is_cell) return 0; /* cell in the virtual row context */
        return 1; /* row/group/col with no tr in table scope */
    case H_TPLM_IN_CGROUP:
        /* gumbo handle_in_column_group: with the template as
         * current node (not a colgroup), every token but a col
         * start is a parse error and ignored (template.dat:71-78). */
        if (is_col) return 0;
        return 1;
        default: /* H_TPLM_IN_BODY: stray table tags drop */
        return 1;
    }
}


/* font breaks out of foreign content only when it carries a
 * color/face/size attribute — peek the raw tag span [q, '>')
 * without consuming. */
static int h_font_break(const char* q, const char* end) {
    while (q < end && *q != '>') {
        while (q < end && (h_is_ws(*q) || *q == '/')) q++;
        const char* as = q;
        while (q < end && !h_is_ws(*q) && *q != '=' && *q != '>' &&
               *q != '/')
            q++;
        size_t alen = (size_t)(q - as);
        if (alen == 5 &&
            (memcmp(as, "color", 5) == 0 ||
             memcmp(as, "COLOR", 5) == 0))
            return 1;
        if (alen == 4 &&
            (memcmp(as, "face", 4) == 0 || memcmp(as, "FACE", 4) == 0))
            return 1;
        if (alen == 4 &&
            (memcmp(as, "size", 4) == 0 || memcmp(as, "SIZE", 4) == 0))
            return 1;
        /* Skip any value. */
        const char* scan = q;
        while (scan < end && h_is_ws(*scan)) scan++;
        if (scan < end && *scan == '=') {
            scan++;
            while (scan < end && h_is_ws(*scan)) scan++;
            if (scan < end && (*scan == '\'' || *scan == '"')) {
                char quote = *scan++;
                while (scan < end && *scan != quote) scan++;
                if (scan < end) scan++;
            } else {
                while (scan < end && !h_is_ws(*scan) && *scan != '>')
                    scan++;
            }
            q = scan;
        }
    }
    return 0;
}

static int h_is_table_context(LeptrisElement e) {
    const char* n = e ? leptris_element_name(e) : NULL;
    return h_ieq_raw(n, "table") || h_ieq_raw(n, "tbody") ||
           h_ieq_raw(n, "thead") || h_ieq_raw(n, "tfoot") ||
           h_ieq_raw(n, "tr");
}

/* Insert n into parent's child chain BEFORE `before`. Same surgery
 * pattern as the head-content lift (first_child + sibling links +
 * element child_count). */
static void h_insert_before(HBuilder* b, LeptrisElement parent,
                            LeptrisElement before, LeptrisNodeRef n) {
    LeptrisNodeRef first = leptris_node_first_child_internal(
        (LeptrisNode*)parent);
    if (first == (LeptrisNodeRef)before) {
        leptris_elem_set_first_child(parent, (LeptrisNodeRef)n);
    } else {
        LeptrisNodeRef prev = first;
        while (prev) {
            LeptrisNodeRef nx = leptris_node_get_next_sibling(prev);
            if (nx == (LeptrisNodeRef)before) break;
            prev = nx;
        }
        if (prev) leptris_node_set_next_sibling(prev, n);
        else { /* before not in chain (shouldn't happen): fall back
                * to a plain append. */
            leptris_element_append_child_internal_doc(parent, n, b->doc);
            return;
        }
    }
    leptris_node_set_next_sibling(n, (LeptrisNodeRef)before);
    if (leptris_node_get_type(n) == LEPTRIS_NODE_TYPE_ELEMENT)
        parent->child_count++;
}

static void h_append(HBuilder* b, LeptrisNodeRef n) {
    if (b->whatwg && !b->left_initial &&
        leptris_node_get_type(n) == LEPTRIS_NODE_TYPE_TEXT) {
        const char* t = leptris_text_node_get_content(n);
        if (t)
            for (const char* q = t; *q; q++)
                if (*q != ' ' && *q != '\t' && *q != '\n' &&
                    *q != '\r') {
                    b->left_initial = 1;
                    break;
                }
    }
    /* #659 in-column-group on a template current node: only col
     * starts live there; every other token, non-whitespace text
     * included, is a parse error and ignored (gumbo
     * handle_in_column_group; html5lib template.dat:76). */
    if (b->whatwg && b->depth > 0 &&
        leptris_node_get_type(n) == LEPTRIS_NODE_TYPE_TEXT &&
        b->tmpl_mode[b->depth - 1] == H_TPLM_IN_CGROUP &&
        h_ieq_raw(leptris_element_name(b->open[b->depth - 1]),
                  "template")) {
        const char* ht = leptris_text_node_get_content(n);
        int nonws = 0;
        if (ht)
            for (const char* hq = ht; *hq; hq++)
                if (*hq != ' ' && *hq != '\t' &&
                    *hq != '\n' && *hq != '\r') {
                    nonws = 1;
                    break;
                }
        if (nonws) return;
    }
    if (b->depth > 0) {
        LeptrisElement top = b->open[b->depth - 1];
        /* #659 foster (WHATWG only): text/elements in table context
         * go before the nearest open table in ITS parent. */
        if (b->whatwg_foster && h_is_table_context(top) &&
            h_fosterable(b, n)) {
            int ti = (int)b->depth - 1;
            while (ti >= 0 && !h_ieq_raw(
                       leptris_element_name(b->open[ti]), "table"))
                ti--;
            if (ti >= 1) {
                LeptrisElement table = b->open[ti];
                LeptrisElement tparent = b->open[ti - 1];
                h_insert_before(b, tparent, table, n);
                return;
            }
            if (ti == 0) {
                /* Table sits directly on the top chain — splice n
                 * into the chain BEFORE the table node. */
                LeptrisElement table = b->open[0];
                if (b->top_head == (LeptrisNodeRef)table) {
                    leptris_node_set_next_sibling(
                        n, b->top_head);
                    b->top_head = n;
                } else {
                    LeptrisNodeRef prev = b->top_head;
                    while (prev) {
                        LeptrisNodeRef nx =
                            leptris_node_get_next_sibling(prev);
                        if (nx == (LeptrisNodeRef)table) break;
                        prev = nx;
                    }
                    if (prev) {
                        leptris_node_set_next_sibling(prev, n);
                        leptris_node_set_next_sibling(
                            n, (LeptrisNodeRef)table);
                    }
                }
                return;
            }
        }
        leptris_element_append_child_internal_doc(top, n, b->doc);
    } else {
        h_top_append(b, n);
    }
}

/* WHATWG bogus comment (13.2.5.41/13.2.5.7): a comment whose
 * data is the raw bytes to the first '>' (or EOF). */
static void h_bogus_comment(HBuilder* b, const char* data,
                            size_t len) {
    /* 13.2.5.41: NUL in bogus-comment data becomes U+FFFD. */
    char* buf = (char*)leptris_pool_alloc(b->pool, 3 * len + 1);
    if (!buf) return;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0) {
            buf[o++] = (char)0xEF;
            buf[o++] = (char)0xBD;
            buf[o++] = (char)0xBF;
        } else {
            buf[o++] = data[i];
        }
    }
    LeptrisCommentNode* c =
        leptris_comment_create(buf, o, b->pool);
    if (!c) return;
    c->owner_doc = b->doc;
    if (b->whatwg && !b->left_initial) {
        /* Initial mode: a Document-level child (the prolog
         * chain), exactly like a leading <!-- comment. */
        if (b->prolog_tail)
            leptris_node_set_next_sibling(b->prolog_tail,
                                          (LeptrisNodeRef)c);
        else
            b->prolog_head = (LeptrisNodeRef)c;
        b->prolog_tail = (LeptrisNodeRef)c;
        return;
    }
    h_append(b, (LeptrisNodeRef)c);
}

/* Pop the open stack down to (and including) index d. */
static void h_pop_to(HBuilder* b, size_t d) {
    b->depth = d;
}

/* Create + attach a foreign element (SVG/MathML namespace URI,
 * case-adjusted name), foster-aware via h_append. */
static LeptrisElement h_open_foreign(HBuilder* b, const char* name,
                                     int ns) {
    const char* store = name;
    const char* adj = (ns == H_NS_SVG) ? h_svg_name(name) : NULL;
    if (adj) store = adj;
    LeptrisStringView nv = leptris_sv_from_cstr(store);
    LeptrisElement e = leptris_element_create_with_view(nv, b->pool);
    if (!e) return NULL;
    leptris_root_doc_register(e, b->doc);
    b->left_initial = 1;
    leptris_element_set_namespace_uri_view(
        e, leptris_sv_from_cstr(
               ns == H_NS_SVG ? H_SVG_URI : H_MATH_URI));
    h_append(b, (LeptrisNodeRef)e);
    if (b->depth == 0 && !b->root) b->root = e;
    if (b->depth < 256) {
        b->open[b->depth] = e;
        b->open_ns[b->depth] = (uint8_t)ns;
        b->depth++;
    }
    return e;
}

static LeptrisElement h_open_element(HBuilder* b, const char* name) {
    LeptrisStringView nv = leptris_sv_from_cstr(name);
    LeptrisElement e = leptris_element_create_with_view(nv, b->pool);
    if (!e) return NULL;
    /* Detached pre-registration: children append through the doc-
     * resolved internal; the root-map entry makes that work before
     * the element is attached (round-20 create contract). */
    leptris_root_doc_register(e, b->doc);
    b->left_initial = 1;
    h_append(b, (LeptrisNodeRef)e);
    if (b->depth == 0 && !b->root) b->root = e;
    if (b->depth < 256) {
        b->open[b->depth] = e;
        b->open_ns[b->depth] = H_NS_HTML;
        b->depth++;
    }
    return e;
}

/* Case-insensitive C-string compare (ASCII). */
static int h_ieq_raw(const char* a, const char* bname) {
    if (!a) return 0;
    while (*a && *bname) {
        if (h_lower(*a) != *bname) return 0;
        a++; bname++;
    }
    return *a == 0 && *bname == 0;
}

/* ---- #659 WHATWG adoption agency (13.2.6.4.7) ----
 * The list of active formatting elements + reconstruct + the
 * agency itself. Stack positions are oldest-first in open[]
 * (open[depth-1] is the current node), which is the spec stack
 * inverted; "above X" = open[i-1], "below X" = open[i+1]. */

static int h_stack_find(HBuilder* b, LeptrisElement e) {
    for (size_t i = 0; i < b->depth; i++)
        if (b->open[i] == e) return (int)i;
    return -1;
}

/* Entry index of el anywhere in the list (-1 if absent). */
static int h_afe_index_of(HBuilder* b, LeptrisElement el) {
    for (int i = 0; i < b->afe_n; i++)
        if (!b->afe_marker[i] && b->afe[i] == el) return i;
    return -1;
}

/* Last entry after the last marker whose name is subject. */
static int h_afe_find(HBuilder* b, const char* subject) {
    int last_marker = -1;
    for (int i = 0; i < b->afe_n; i++)
        if (b->afe_marker[i]) last_marker = i;
    for (int i = b->afe_n - 1; i > last_marker; i--)
        if (!b->afe_marker[i]) {
            const char* n = leptris_element_name(b->afe[i]);
            if (n && strcmp(n, subject) == 0) return i;
        }
    return -1;
}

static void h_afe_remove_idx(HBuilder* b, int idx) {
    if (idx < 0 || idx >= b->afe_n) return;
    memmove(&b->afe[idx], &b->afe[idx + 1],
            (b->afe_n - idx - 1) * sizeof(b->afe[0]));
    memmove(&b->afe_marker[idx], &b->afe_marker[idx + 1],
            (b->afe_n - idx - 1));
    b->afe_n--;
}

static void h_afe_clear_to_marker(HBuilder* b) {
    while (b->afe_n > 0) {
        int marker = b->afe_marker[b->afe_n - 1];
        b->afe_n--;
        if (marker) break;
    }
}

/* Same token identity (name + attribute multiset) — the Noah's
 * Ark clause compares attributes as parsed. */
static int h_fmt_same(LeptrisElement a, LeptrisElement b2) {
    if (a == b2) return 1;
    const char* na = leptris_element_name(a);
    const char* nb = leptris_element_name(b2);
    if (!na || !nb || strcmp(na, nb) != 0) return 0;
    int ca = 0, cb = 0;
    for (struct leptris_attribute* x =
             leptris_element_get_first_attribute(a);
         x; x = leptris_attr_next(x))
        ca++;
    for (struct leptris_attribute* x =
             leptris_element_get_first_attribute(b2);
         x; x = leptris_attr_next(x))
        cb++;
    if (ca != cb) return 0;
    for (struct leptris_attribute* x =
             leptris_element_get_first_attribute(a);
         x; x = leptris_attr_next(x)) {
        int found = 0;
        for (struct leptris_attribute* y =
                 leptris_element_get_first_attribute(b2);
             y; y = leptris_attr_next(y))
            if (strcmp(attr_cname(x), attr_cname(y)) == 0 &&
                strcmp(attr_cvalue(x), attr_cvalue(y)) == 0) {
                found = 1;
                break;
            }
        if (!found) return 0;
    }
    return 1;
}

/* Push with the Noah's Ark clause: a fourth same-identity entry
 * after the last marker drops the earliest one. */
static void h_afe_push(HBuilder* b, LeptrisElement e) {
    int last_marker = -1;
    for (int i = 0; i < b->afe_n; i++)
        if (b->afe_marker[i]) last_marker = i;
    int same = 0, earliest = -1;
    for (int i = last_marker + 1; i < b->afe_n; i++) {
        if (b->afe_marker[i]) break;
        if (h_fmt_same(e, b->afe[i])) {
            same++;
            if (earliest < 0) earliest = i;
        }
    }
    if (same >= 3 && earliest >= 0) h_afe_remove_idx(b, earliest);
    if (b->afe_n >= 64) return;
    b->afe[b->afe_n] = e;
    b->afe_marker[b->afe_n] = 0;
    b->afe_n++;
}

static void h_afe_marker_push(HBuilder* b) {
    if (b->afe_n >= 64) return;
    b->afe[b->afe_n] = NULL;
    b->afe_marker[b->afe_n] = 1;
    b->afe_n++;
}

/* Unattached re-creation of an element for the token it was
 * created from (13.2.6.1): same name + parsed attributes. */
static LeptrisElement h_afe_clone(HBuilder* b, LeptrisElement src) {
    const char* n = leptris_element_name(src);
    if (!n) return NULL;
    LeptrisElement c =
        leptris_element_create_with_view(leptris_sv_from_cstr(n),
                                         b->pool);
    if (!c) return NULL;
    leptris_root_doc_register(c, b->doc);
    for (struct leptris_attribute* a =
             leptris_element_get_first_attribute(src);
         a; a = leptris_attr_next(a))
        leptris_element_add_attribute(
            c, leptris_sv_from_cstr(attr_cname(a)),
            leptris_sv_from_cstr(attr_cvalue(a)), b->pool);
    return c;
}

/* Attach + push one reconstruction clone at the current
 * insertion point (append_child unlinks from any old parent). */
static LeptrisElement h_afe_open_clone(HBuilder* b, LeptrisElement src) {
    const char* n = leptris_element_name(src);
    if (!n) return NULL;
    LeptrisElement c =
        leptris_element_create_with_view(leptris_sv_from_cstr(n),
                                         b->pool);
    if (!c) return NULL;
    leptris_root_doc_register(c, b->doc);
    for (struct leptris_attribute* a =
             leptris_element_get_first_attribute(src);
         a; a = leptris_attr_next(a))
        leptris_element_add_attribute(
            c, leptris_sv_from_cstr(attr_cname(a)),
            leptris_sv_from_cstr(attr_cvalue(a)), b->pool);
    h_append(b, (LeptrisNodeRef)c);
    if (b->depth == 0 && !b->root) b->root = c;
    if (b->depth < 256) {
        b->open[b->depth] = c;
        b->open_ns[b->depth] = H_NS_HTML;
        b->depth++;
    }
    return c;
}

/* Reconstruct the active formatting elements (13.2.4.3): every
 * entry after the last in-stack/marker entry re-opens as a fresh
 * clone at the insertion point. Character and ordinary start
 * tags run this before inserting. */
static void h_reconstruct(HBuilder* b) {
    if (!b->whatwg_adopt || b->afe_n == 0) return;
    /* Raw-text containers hold raw data ("text" insertion mode);
     * no reconstruction runs inside them. */
    if (b->depth > 0) {
        const char* tn = leptris_element_name(b->open[b->depth - 1]);
        if (tn &&
            (strcmp(tn, "title") == 0 || strcmp(tn, "textarea") == 0 ||
             strcmp(tn, "script") == 0 || strcmp(tn, "style") == 0 ||
             strcmp(tn, "xmp") == 0 || strcmp(tn, "iframe") == 0 ||
             strcmp(tn, "noembed") == 0 || strcmp(tn, "noscript") == 0))
            return;
    }
    int i = b->afe_n - 1;
    if (b->afe_marker[i] || h_stack_find(b, b->afe[i]) >= 0) return;
    while (i > 0 && !b->afe_marker[i - 1] &&
           h_stack_find(b, b->afe[i - 1]) < 0)
        i--;
    for (int j = i; j < b->afe_n; j++) {
        LeptrisElement c = h_afe_open_clone(b, b->afe[j]);
        if (c) b->afe[j] = c;
    }
}

/* The adoption agency proper. Returns 1 when the end tag is fully
 * consumed; 0 means no formatting entry matched and the caller
 * runs its generic (any-other-end-tag) handling. */
static int h_afe_end(HBuilder* b, const char* subject) {
    if (b->depth > 0) {
        LeptrisElement cur = b->open[b->depth - 1];
        const char* cn = leptris_element_name(cur);
        if (cn && strcmp(cn, subject) == 0 &&
            h_afe_index_of(b, cur) < 0) {
            b->depth--;
            return 1;
        }
    }
    for (int outer = 0; outer < 8; outer++) {
        int fi = h_afe_find(b, subject);
        if (fi < 0) return 0;
        LeptrisElement fe = b->afe[fi];
        int si = h_stack_find(b, fe);
        if (si < 0) {
            h_afe_remove_idx(b, fi);
            return 1;
        }
        int fbi = -1;
        for (size_t k = (size_t)si + 1; k < b->depth; k++) {
            /* Only HTML-namespace elements are furthest-block
             * candidates — foreign elements with special-looking
             * names (svg tr) are ordinary foreign content. */
            if (b->open_ns[k] == H_NS_HTML &&
                h_is_special_ww(leptris_element_name(b->open[k]))) {
                fbi = (int)k;
                break;
            }
        }
        if (fbi < 0) {
            b->depth = (size_t)si;
            h_afe_remove_idx(b, fi);
            return 1;
        }
        LeptrisElement fb = b->open[fbi];
        LeptrisElement ancestor =
            si > 0 ? b->open[si - 1] : NULL;   /* NULL = top chain */
        int bookmark = fi;
        /* Pre-removal stack snapshot: the inner loop walks the
         * neighbors nodes had before this algorithm removed any
         * of them. */
        LeptrisElement snap[256];
        size_t snapn = b->depth;
        memcpy(snap, b->open, snapn * sizeof(snap[0]));
        LeptrisElement node = fb, last = fb;
        int inner = 0;
        for (;;) {
            inner++;
            int ni = -1;
            for (size_t k = 0; k < snapn; k++)
                if (snap[k] == node) {
                    ni = (int)k;
                    break;
                }
            if (ni <= 0) break;
            LeptrisElement above = snap[ni - 1];
            if (above == fe) break;
            int ai = h_afe_index_of(b, above);
            if (inner > 3 && ai >= 0) {
                h_afe_remove_idx(b, ai);
                if (bookmark > ai) bookmark--;
                ai = -1;
            }
            if (ai < 0) {
                /* Not a formatting entry: drop it from the real
                 * stack; the snapshot keeps its old position for
                 * navigation. */
                int ri = h_stack_find(b, above);
                if (ri >= 0) {
                    memmove(&b->open[ri], &b->open[ri + 1],
                            (b->depth - ri - 1) *
                                sizeof(b->open[0]));
                    memmove(&b->open_ns[ri], &b->open_ns[ri + 1],
                            (b->depth - ri - 1));
                    b->depth--;
                }
                node = above;
                continue;
            }
            LeptrisElement cl = h_afe_clone(b, above);
            if (!cl) break;
            b->afe[ai] = cl;
            int ri = h_stack_find(b, above);
            if (ri >= 0) b->open[ri] = cl;
            snap[ni - 1] = cl;
            if (last == fb) bookmark = ai + 1;
            leptris_element_append_child_internal_doc(
                cl, (LeptrisNodeRef)last, b->doc);
            last = node = cl;
        }
        {
            /* Steps 14-16 run even when the inner loop broke at
             * once (lastNode == furthestBlock): moving the block
             * out of the formatting element IS the adoption. The
             * appropriate place is a plain append, or foster
             * parenting when the ancestor is a table context. */
            if (ancestor &&
                h_is_table_context(ancestor)) {
                int ti = (int)b->depth - 1;
                while (ti >= 0 && !h_ieq_raw(
                            leptris_element_name(b->open[ti]),
                            "table"))
                    ti--;
                if (ti >= 1) {
                    h_insert_before(b, b->open[ti - 1],
                                    b->open[ti], (LeptrisNodeRef)last);
                } else {
                    /* Top-chain splice before the table node —
                     * unlink from the old parent first. */
                    leptris_node_unlink((LeptrisNodeRef)last);
                    if (b->top_head) {
                        if (b->top_head ==
                            (LeptrisNodeRef)b->open[0]) {
                            leptris_node_set_next_sibling(
                                (LeptrisNodeRef)last, b->top_head);
                            b->top_head = (LeptrisNodeRef)last;
                        } else {
                            LeptrisNodeRef prev = b->top_head;
                            while (prev) {
                                LeptrisNodeRef nx =
                                    leptris_node_get_next_sibling(
                                        prev);
                                if (nx == (LeptrisNodeRef)b->open[0])
                                    break;
                                prev = nx;
                            }
                            if (prev) {
                                leptris_node_set_next_sibling(
                                    prev, (LeptrisNodeRef)last);
                                leptris_node_set_next_sibling(
                                    (LeptrisNodeRef)last,
                                    (LeptrisNodeRef)b->open[0]);
                            }
                        }
                    }
                }
            } else if (ancestor) {
                leptris_element_append_child_internal_doc(
                    ancestor, (LeptrisNodeRef)last, b->doc);
            } else {
                /* Top chain: unlink from the old parent first —
                 * h_top_append only links siblings. */
                leptris_node_unlink((LeptrisNodeRef)last);
                h_top_append(b, (LeptrisNodeRef)last);
            }
        }
        /* New formatting element inside the furthest block: it
         * adopts the block's children, then is appended. */
        LeptrisElement ne = h_afe_clone(b, fe);
        if (!ne) return 1;
        LeptrisNodeRef c = leptris_node_first_child_internal(
            (LeptrisNode*)fb);
        while (c) {
            LeptrisNodeRef nx = leptris_node_get_next_sibling(c);
            leptris_element_append_child_internal_doc(
                ne, c, b->doc);
            c = nx;
        }
        leptris_element_append_child_internal_doc(
            fb, (LeptrisNodeRef)ne, b->doc);
        h_afe_remove_idx(b, fi);
        if (bookmark > fi) bookmark--;
        if (bookmark > b->afe_n) bookmark = b->afe_n;
        if (b->afe_n < 64) {
            memmove(&b->afe[bookmark + 1], &b->afe[bookmark],
                    (b->afe_n - bookmark) * sizeof(b->afe[0]));
            memmove(&b->afe_marker[bookmark + 1],
                    &b->afe_marker[bookmark],
                    (b->afe_n - bookmark));
            b->afe[bookmark] = ne;
            b->afe_marker[bookmark] = 0;
            b->afe_n++;
        }
        int ri = h_stack_find(b, fe);
        if (ri >= 0) {
            memmove(&b->open[ri], &b->open[ri + 1],
                    (b->depth - ri - 1) * sizeof(b->open[0]));
            memmove(&b->open_ns[ri], &b->open_ns[ri + 1],
                    (b->depth - ri - 1));
            b->depth--;
        }
        /* Insert the new element immediately below (more recent
         * than) the furthest block's stack position. */
        int fbpos = h_stack_find(b, fb);
        if (fbpos >= 0 && b->depth < 256) {
            memmove(&b->open[fbpos + 2], &b->open[fbpos + 1],
                    (b->depth - fbpos - 1) * sizeof(b->open[0]));
            memmove(&b->open_ns[fbpos + 2],
                    &b->open_ns[fbpos + 1],
                    (b->depth - fbpos - 1));
            b->open[fbpos + 1] = ne;
            b->open_ns[fbpos + 1] = H_NS_HTML;
            b->depth++;
        }
    }
    return 1;
}

/* Create + attach an element; push==0 leaves the stack alone
 * (synthesis helpers run at commit time). */
static LeptrisElement h_open_named(HBuilder* b, const char* name,
                                   int push) {
    LeptrisStringView nv = leptris_sv_from_cstr(name);
    LeptrisElement e = leptris_element_create_with_view(nv, b->pool);
    if (!e) return NULL;
    leptris_root_doc_register(e, b->doc);
    h_append(b, (LeptrisNodeRef)e);
    if (b->depth == 0 && !b->root) b->root = e;
    if (push && b->depth < 256) {
        b->open[b->depth] = e;
        b->open_ns[b->depth] = H_NS_HTML;
        b->depth++;
    }
    return e;
}

/* Create + attach an element directly under an explicit parent
 * (synthesis: head/body under html, off the open-element stack). */
static LeptrisElement h_new_child(HBuilder* b, LeptrisElement parent,
                                  const char* name) {
    LeptrisStringView nv = leptris_sv_from_cstr(name);
    LeptrisElement e = leptris_element_create_with_view(nv, b->pool);
    if (!e) return NULL;
    leptris_root_doc_register(e, b->doc);
    leptris_element_append_child_internal_doc(parent, (LeptrisNodeRef)e,
                                              b->doc);
    return e;
}


/* #659 head/body split over `html`'s current child chain: lift
 * the leading head run (bounded by the structural-body marker)
 * into a <head> spliced in as first child; move the rest into a
 * <body> appended last — unless an explicit <body> element is
 * already among the rest (explicit html), where the rest stays.
 * The chain stays attached throughout (no detach/reattach). */
static LeptrisElement h_create_unattached(HBuilder* b, const char* name) {
    LeptrisStringView nv = leptris_sv_from_cstr(name);
    LeptrisElement e = leptris_element_create_with_view(nv, b->pool);
    if (e) leptris_root_doc_register(e, b->doc);
    return e;
}

static void h_split_head_body(HBuilder* b, LeptrisElement html,
                              LeptrisNodeRef orig_head) {
    LeptrisNodeRef head_end = orig_head;   /* first non-head node */
    if (!(b->lift_closed && !b->lift_boundary)) {
        while (head_end) {
            /* WHATWG "in head": comments (and PI-ish bogus
             * comments) are head children — neutral in the run,
             * never ending it. html4 keeps libxml2's shape (they
             * ride in body). */
            int hty = leptris_node_get_type(head_end);
            if (hty == LEPTRIS_NODE_TYPE_COMMENT ||
                hty == LEPTRIS_NODE_TYPE_PI) {
                if (b->whatwg_head_set) {
                    head_end = leptris_node_get_next_sibling(head_end);
                    continue;
                }
                break;
            }
            if (hty != LEPTRIS_NODE_TYPE_ELEMENT)
                break;
            const char* hn = leptris_element_name((LeptrisElement)head_end);
            /* #659 two modes: WHATWG lifts the full "in head" set;
             * the html4-compat entry lifts only title/meta/link/
             * base (libxml2 leaves leading script/style in body). */
            int head_el =
                h_ieq_raw(hn, "title") || h_ieq_raw(hn, "meta") ||
                h_ieq_raw(hn, "link") || h_ieq_raw(hn, "base");
            if (b->whatwg_head_set)
                head_el = head_el || h_ieq_raw(hn, "basefont") ||
                          h_ieq_raw(hn, "bgsound") ||
                          h_ieq_raw(hn, "script") ||
                          h_ieq_raw(hn, "style") ||
                          h_ieq_raw(hn, "noframes") ||
                          h_ieq_raw(hn, "noscript") ||
                          h_ieq_raw(hn, "template");
            if (!head_el)
                break;
            head_end = leptris_node_get_next_sibling(head_end);
            /* A structural <body> ends the head phase. */
            if (b->lift_boundary && head_end == b->lift_boundary) {
                head_end = leptris_node_get_next_sibling(head_end);
                break;
            }
        }
    }
    /* A non-empty run hands the tail to the body (possibly NULL
     * when the run consumed everything); an empty run keeps the
     * whole chain as the rest. */
    LeptrisNodeRef rest = (head_end == orig_head) ? orig_head : head_end;

    /* An explicit <body> among the rest keeps the rest in place. */
    for (LeptrisNodeRef c = rest; c;
         c = leptris_node_get_next_sibling(c)) {
        if (leptris_node_get_type(c) == LEPTRIS_NODE_TYPE_ELEMENT &&
            h_ieq_raw(leptris_element_name((LeptrisElement)c), "body"))
            return;
    }

    /* <head> spliced in as the first child, owning the run. */
    LeptrisElement html_first_new = NULL;
    int head_spliced = 0;
    if (head_end != orig_head) {
        LeptrisElement head = h_create_unattached(b, "head");
        if (head) {
            head_spliced = 1;
            html_first_new = head;
            size_t hn = 0;
            LeptrisNodeRef hlast = NULL;
            for (LeptrisNodeRef c = orig_head; c && c != head_end; ) {
                LeptrisNodeRef next = leptris_node_get_next_sibling(c);
                /* The run can carry comments (WHATWG in-head) —
                 * set_parent must go through the node-kind setter,
                 * an element-shaped write corrupts them. */
                switch (leptris_node_get_type(c)) {
                    case LEPTRIS_NODE_TYPE_COMMENT:
                        leptris_comment_set_parent((LeptrisCommentNode*)c,
                                                   head);
                        break;
                    case LEPTRIS_NODE_TYPE_TEXT:
                        leptris_textnode_set_parent((LeptrisTextNode*)c,
                                                    head);
                        break;
                    case LEPTRIS_NODE_TYPE_CDATA:
                        leptris_cdata_set_parent((LeptrisCDATANode*)c, head);
                        break;
                    case LEPTRIS_NODE_TYPE_PI:
                        leptris_pi_set_parent((LeptrisPINode*)c, head);
                        break;
                    default:
                        leptris_element_set_parent((LeptrisElement)c, head);
                        break;
                }
                hlast = c;
                hn++;
                c = next;
            }
            leptris_elem_set_first_child(head, orig_head);
            leptris_elem_set_last_child(head, hlast);
            head->child_count = hn;
            if (hlast) leptris_node_set_next_sibling(hlast, NULL);
            leptris_node_set_next_sibling((LeptrisNodeRef)head, rest);
            leptris_elem_set_first_child(html, (LeptrisNodeRef)head);
            leptris_element_set_parent(head, html);
        }
    }

    /* <body> (or <frameset>, #659) owns the rest, then links in
     * as html's last child. When the rest already IS a parsed
     * frameset (the replace-body path), it stays as its own
     * wrapper — no synthesized shell around it. */
    LeptrisElement body = NULL;
    if (b->frameset && rest) {
        LeptrisNodeRef c0 = rest;
        while (c0 && leptris_node_get_type(c0) !=
                        LEPTRIS_NODE_TYPE_ELEMENT)
            c0 = leptris_node_get_next_sibling(c0);
        if (c0 && h_ieq_raw(leptris_element_name((LeptrisElement)c0),
                            "frameset"))
            body = (LeptrisElement)c0;
    }
    if (body) {
        /* Already owns its children and its place in html's
         * chain — relink defensively and recount. */
        leptris_element_set_parent(body, html);
        if (head_spliced)
            leptris_node_set_next_sibling(
                (LeptrisNodeRef)html_first_new, (LeptrisNodeRef)body);
        else
            leptris_elem_set_first_child(html, (LeptrisNodeRef)body);
        size_t cnt = 0;
        for (LeptrisNodeRef c =
                 leptris_node_first_child((LeptrisNodeRef)html);
             c; c = leptris_node_get_next_sibling(c))
            cnt++;
        html->child_count = cnt;
        return;
    }
    body = h_create_unattached(b, b->frameset ? "frameset" : "body");
    if (!body) return;
    size_t elems = 0;
    LeptrisNodeRef last = NULL;
    for (LeptrisNodeRef c = rest; c; ) {
        LeptrisNodeRef next = leptris_node_get_next_sibling(c);
        int ty = leptris_node_get_type(c);
        if (ty == LEPTRIS_NODE_TYPE_ELEMENT) {
            leptris_element_set_parent((LeptrisElement)c, body);
            elems++;
        } else if (ty == LEPTRIS_NODE_TYPE_TEXT) {
            leptris_textnode_set_parent((LeptrisTextNode*)c, body);
        } else if (ty == LEPTRIS_NODE_TYPE_COMMENT) {
            leptris_comment_set_parent((LeptrisCommentNode*)c, body);
        } else if (ty == LEPTRIS_NODE_TYPE_CDATA) {
            leptris_cdata_set_parent((LeptrisCDATANode*)c, body);
        } else if (ty == LEPTRIS_NODE_TYPE_PI) {
            leptris_pi_set_parent((LeptrisPINode*)c, body);
        }
        last = c;
        c = next;
    }
    leptris_elem_set_first_child(body, rest);
    body->child_count = elems;
    if (last) leptris_node_set_next_sibling(last, NULL);
    /* html's chain is now exactly [head?, body] — the rest left
     * html when it moved into body. */
    if (head_spliced)
        leptris_node_set_next_sibling((LeptrisNodeRef)html_first_new,
                                      (LeptrisNodeRef)body);
    else
        leptris_elem_set_first_child(html, (LeptrisNodeRef)body);
    leptris_element_set_parent(body, html);
    html->child_count = 0;
    if (head_spliced) html->child_count++;
    html->child_count++;
}

/* ---- tokenizer ---- */
static LeptrisDocument html_parse_shared(
    const char* buf, size_t len, LeptrisStatus* status, int whatwg) {
    if (status) *status = LEPTRIS_OK;
    if (!buf) {
        if (status) *status = LEPTRIS_ERROR_NULL_ARG;
        return NULL;
    }
    struct leptris_document* doc =
        (struct leptris_document*)leptris_document_create();
    if (!doc) {
        if (status) *status = LEPTRIS_ERROR_MEMORY;
        return NULL;
    }
    HBuilder b;
    memset(&b, 0, sizeof(b));
    b.doc = doc;
    b.pool = doc->pool;
    b.whatwg = whatwg;
    b.whatwg_head_set = whatwg;
    b.whatwg_foster = whatwg;
    b.whatwg_adopt = whatwg;

    const char* p = buf;
    const char* end = buf + len;
    const char* text = p;   /* pending text run start */

    while (p < end) {
        if (*p != '<') { p++; continue; }

        /* Classify the markup construct. */
        if (p + 1 >= end) { p++; continue; }
        char k1 = p[1];

        if (k1 == '!') {
            /* Flush pending text first. */
            if (text < p) {
                char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
                if (dec && *dec) {
                    LeptrisTextNode* t = leptris_text_create(
                        dec, strlen(dec), b.pool);
                    if (t) {
                        h_reconstruct(&b);
                        h_append(&b, (LeptrisNodeRef)t);
                    }
                }
            }
            if (p + 3 < end && p[2] == '-' && p[3] == '-') {
                /* #659 WHATWG 12.2.5.5x: comments close at -->
                 * OR the abrupt form --!>; unterminated comments
                 * run to EOF (data verbatim). <!--> and <!--->
                 * are empty comments. */
                const char* cs = p + 4;
                const char* ce = cs;
                size_t cclose = 0;   /* close-marker length */
                if (cs < end && *cs == '>') {
                    cclose = 1;   /* <!--> */
                } else if (cs + 1 < end && cs[0] == '-' &&
                           cs[1] == '>') {
                    cclose = 2;   /* <!---> */
                } else {
                    while (ce + 3 <= end) {
                        if (ce[0] == '-' && ce[1] == '-') {
                            if (ce[2] == '>') {
                                cclose = 3;
                                break;
                            }
                            if (ce + 3 < end && ce[2] == '!' &&
                                ce[3] == '>') {
                                cclose = 4;   /* --!> */
                                break;
                            }
                        }
                        ce++;
                    }
                    if (!cclose) ce = end;   /* unterminated */
                }
                size_t clen = cclose
                                  ? (size_t)(ce - cs)
                                  : (size_t)(end - cs);
                p = cclose ? ce + cclose : end;
                LeptrisCommentNode* c = leptris_comment_create(
                    cs, clen, b.pool);
                if (c) {
                    c->owner_doc = doc;
                    if (b.whatwg && !b.left_initial) {
                        /* Initial mode: a Document-level child. */
                        if (b.prolog_tail)
                            leptris_node_set_next_sibling(
                                b.prolog_tail, (LeptrisNodeRef)c);
                        else
                            b.prolog_head = (LeptrisNodeRef)c;
                        b.prolog_tail = (LeptrisNodeRef)c;
                    } else {
                        h_append(&b, (LeptrisNodeRef)c);
                    }
                }
            } else {
                /* First <!doctype ...> is recorded on the document
                 * (name + legacy PUBLIC/SYSTEM ids), like the XML
                 * path; later ones and other <!...> constructs are
                 * just skipped. */
                const char* q = p + 2;
                int is_dt = 0;
                if (end - q >= 7 && !doc->doctype &&
                    !h_in_head_noscript(&b)) {
                    static const char kw[] = "doctype";
                    is_dt = 1;
                    for (int i = 0; i < 7; i++) {
                        if (h_lower(q[i]) != kw[i]) {
                            is_dt = 0;
                            break;
                        }
                    }
                }
                if (is_dt) {
                    q += 7;
                    while (q < end && h_is_ws(*q)) q++;
                    const char* nstart = q;
                    while (q < end && !h_is_ws(*q) && *q != '>') q++;
                    size_t nlen = (size_t)(q - nstart);

                    /* Legacy external ids: keyword, then one
                     * (SYSTEM) or two (PUBLIC) quoted strings. */
                    char pub[256] = {0};
                    char sys[256] = {0};
                    int last_kw = 0;   /* 1 = PUBLIC, 2 = SYSTEM */
                    const char* s = q;
                    while (s < end && *s != '>') {
                        if (h_is_ws(*s)) {
                            s++;
                            continue;
                        }
                        if (end - s >= 6) {
                            int pub_kw = 1, sys_kw = 1;
                            static const char pkw[] = "public";
                            static const char skw[] = "system";
                            for (int i = 0; i < 6; i++) {
                                if (h_lower(s[i]) != pkw[i]) pub_kw = 0;
                                if (h_lower(s[i]) != skw[i]) sys_kw = 0;
                            }
                            if (pub_kw) {
                                last_kw = 1;
                                s += 6;
                                continue;
                            }
                            if (sys_kw) {
                                last_kw = 2;
                                s += 6;
                                continue;
                            }
                        }
                        if ((*s == '"' || *s == '\'') && last_kw) {
                            char quote = *s++;
                            const char* vs = s;
                            while (s < end && *s != quote) s++;
                            size_t vl = (size_t)(s - vs);
                            if (s < end) s++;
                            if (last_kw == 1 && !pub[0] &&
                                vl < sizeof(pub)) {
                                memcpy(pub, vs, vl);
                                pub[vl] = 0;
                                last_kw = 2;   /* 2nd quoted = system */
                            } else if (!sys[0] && vl < sizeof(sys)) {
                                memcpy(sys, vs, vl);
                                sys[vl] = 0;
                                last_kw = 0;
                            }
                            continue;
                        }
                        s++;
                    }
                    while (q < end && *q != '>') q++;
                    p = (q < end) ? q + 1 : end;

                    if (nlen) {
                        /* WHATWG lowercases the doctype name;
                         * libxml2 (the html4/parity mode) preserves
                         * the case as written. */
                        const char* dname = nstart;
                        char lname[64];
                        if (b.whatwg_head_set) {
                            size_t ln = nlen;
                            if (ln >= sizeof(lname)) ln = sizeof(lname) - 1;
                            for (size_t i = 0; i < ln; i++)
                                lname[i] = h_lower(nstart[i]);
                            lname[ln] = 0;
                            dname = lname;
                            nlen = ln;
                        }
                        LeptrisDoctypeNode* dt = leptris_doctype_create(
                            dname, nlen, b.pool);
                        if (dt) {
                            if (pub[0])
                                leptris_doctype_set_public_id(
                                    dt, pub, b.pool);
                            if (sys[0])
                                leptris_doctype_set_system_id(
                                    dt, sys, b.pool);
                            doc->doctype = dt;
                        }
                    }
                    text = p;
                    continue;
                }
                /* #659: CDATA in foreign content is TEXT (raw, no
                 * entity decoding); in HTML content it stays the
                 * bogus-comment skip below. */
                if (b.whatwg && h_cur_ns(&b) != H_NS_HTML) {
                    const char* cs = p + 9;   /* past "<![CDATA[" */
                    const char* ce = cs;
                    while (ce + 3 <= end && !(ce[0] == ']' &&
                                              ce[1] == ']' &&
                                              ce[2] == '>'))
                        ce++;
                    size_t clen = (ce + 3 <= end)
                                      ? (size_t)(ce - cs)
                                      : (size_t)(end - cs);
                    if (clen) {
                        LeptrisTextNode* t = leptris_text_create(
                            cs, clen, b.pool);
                        if (t) h_append(&b, (LeptrisNodeRef)t);
                    }
                    p = (ce + 3 <= end) ? ce + 3 : end;
                } else {
                    /* CDATA-ish bogus: skip to '>'. WHATWG makes
                     * it a bogus comment (tests1:42-49) — except
                     * doctype-shaped constructs, which the
                     * head-noscript/second-doctype rules ignore
                     * (noscript01:1); the html4 entry keeps the
                     * libxml2 drop. */
                    const char* q2 = p + 2;
                    while (q2 < end && *q2 != '>') q2++;
                    int dt_shaped = end - (p + 2) >= 7;
                    if (dt_shaped) {
                        static const char kw[] = "doctype";
                        for (int i = 0; i < 7; i++)
                            if (h_lower(p[2 + i]) != kw[i])
                                dt_shaped = 0;
                    }
                    if (b.whatwg && !dt_shaped) {
                        h_bogus_comment(&b, p + 2,
                                        (size_t)(q2 - (p + 2)));
                    }
                    p = (q2 < end) ? q2 + 1 : end;
                }
            }
            text = p;
            continue;
        }

        if (k1 == '/') {
            /* #659 WHATWG tokenizer tails: EOF right after "</"
             * emits the two characters as text (eof-before-tag-
             * name — h_append's own non-ws check leaves the
             * initial mode); an invalid first tag-name char makes
             * a bogus comment BEFORE the mode flip, so it rides
             * the document prolog like any initial-mode comment
             * (tests1:38-49). html4 keeps its shape. */
            {
                const char* ns2 = p + 2;
                if (b.whatwg && ns2 >= end) {
                    if (text < p) {
                        char* dec = h_decode_ww(b.pool, text, p, 0,
                                                b.whatwg);
                        if (dec && *dec) {
                            LeptrisTextNode* t = leptris_text_create(
                                dec, strlen(dec), b.pool);
                            if (t) h_append(&b, (LeptrisNodeRef)t);
                        }
                    }
                    LeptrisTextNode* t =
                        leptris_text_create("</", 2, b.pool);
                    if (t) h_append(&b, (LeptrisNodeRef)t);
                    p = end;
                    text = p;
                    continue;
                }
                if (b.whatwg && !((*ns2 >= 'a' && *ns2 <= 'z') ||
                                  (*ns2 >= 'A' && *ns2 <= 'Z'))) {
                    if (text < p) {
                        char* dec = h_decode_ww(b.pool, text, p, 0,
                                                b.whatwg);
                        if (dec && *dec) {
                            LeptrisTextNode* t = leptris_text_create(
                                dec, strlen(dec), b.pool);
                            if (t) h_append(&b, (LeptrisNodeRef)t);
                        }
                    }
                    const char* q2 = ns2;
                    while (q2 < end && *q2 != '>') q2++;
                    h_bogus_comment(&b, ns2, (size_t)(q2 - ns2));
                    p = (q2 < end) ? q2 + 1 : end;
                    text = p;
                    continue;
                }
            }
            /* End tag: ends the initial insertion mode too. */
            b.left_initial = 1;
            /* End tag: name, then skip to '>'. */
            const char* ns = p + 2;
            const char* q = ns;
            while (q < end && !h_is_ws(*q) && *q != '>' && *q != '/') q++;
            size_t nlen = (size_t)(q - ns);
            while (q < end && *q != '>') q++;
            if (q < end) q++;
            /* Flush pending text before closing. */
            if (text < p) {
                char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
                if (dec && *dec) {
                    LeptrisTextNode* t = leptris_text_create(
                        dec, strlen(dec), b.pool);
                    if (t) {
                        h_reconstruct(&b);
                        h_append(&b, (LeptrisNodeRef)t);
                    }
                }
            }
            if (nlen && b.depth > 0) {
                /* Find the matching open element (nearest first);
                 * void-element end tags are ignored. */
                char lname[24];
                size_t cl = nlen < sizeof(lname) - 1
                                ? nlen : sizeof(lname) - 1;
                for (size_t i = 0; i < cl; i++)
                    lname[i] = h_lower(ns[i]);
                lname[cl] = 0;
                /* #659 (WHATWG): </br> is treated as <br>. */
                if (b.whatwg && strcmp(lname, "br") == 0) {
                    h_open_element(&b, "br");
                    b.depth--;   /* br is void */
                    p = q;
                    text = p;
                    continue;
                }
                if (!h_is_void(lname)) {
                    /* #659 (WHATWG): heading end tags pop through
                     * the NEAREST heading (any h1-h6), not just
                     * the same name — the rest is normal matching. */
                    if (b.whatwg && h_is_heading(lname)) {
                        int popped = 0;
                        for (size_t d = b.depth; d > 0; d--) {
                            const char* hn =
                                leptris_element_name(b.open[d - 1]);
                            if (hn && h_is_heading(hn)) {
                                h_pop_to(&b, d - 1);
                                popped = 1;
                                break;
                            }
                        }
                        if (popped) {
                            p = q;
                            text = p;
                            continue;
                        }
                    }
                    /* #659 adoption agency (WHATWG 13.2.6.4.7):
                     * formatting end tags run the agency — it
                     * consumes the tag when a formatting entry
                     * matched; otherwise the generic path below
                     * is the any-other-end-tag run. */
                    if (b.whatwg_adopt && h_is_formatting(lname) &&
                        h_afe_end(&b, lname)) {
                        p = q;
                        text = p;
                        continue;
                    }
                    /* #659 in-select end tags: option/optgroup/
                     * select take the generic path; </table>
                     * closes the select first (in select in
                     * table) and reprocesses; the rest drop. */
                    if (b.whatwg && h_in_select(&b)) {
                        if (strcmp(lname, "table") == 0) {
                            for (size_t d2 = b.depth; d2 > 0; d2--)
                                if (strcmp(leptris_element_name(
                                               b.open[d2 - 1]),
                                           "select") == 0) {
                                    b.depth = d2 - 1;
                                    break;
                                }
                        } else if (strcmp(lname, "option") != 0 &&

                                   strcmp(lname, "optgroup") != 0 &&

                                   strcmp(lname, "select") != 0 &&

                                   strcmp(lname, "template") != 0) {
                            p = q;
                            text = p;
                            continue;
                        }
                    }
                    for (size_t d = b.depth; d > 0; d--) {
                        const char* on = leptris_element_name(b.open[d - 1]);
                        /* #659: foreign slots store the
                         * case-ADJUSTED name (foreignObject) —
                         * match the raw source case-insensitively;
                         * HTML slots stay exact lowercase. */
                        int tag_match = 0;
                        if (on) {
                            if (b.whatwg &&
                                strcmp(lname, "template") == 0) {
                                /* </template> follows the in-head rules: only an
                                 * HTML-namespace template matches - an SVG
                                 * template element is foreign content
                                 * (template.dat:100). */
                                tag_match =
                                    b.open_ns[d - 1] == H_NS_HTML &&
                                    strcmp(on, "template") == 0;
                            } else if (b.open_ns[d - 1] != H_NS_HTML) {
                                size_t ol = strlen(on);
                                tag_match = ol == nlen;
                                for (size_t i = 0;
                                     tag_match && i < nlen; i++)
                                    if (h_lower(ns[i]) !=
                                        h_lower(on[i]))
                                        tag_match = 0;
                            } else {
                                tag_match = strcmp(on, lname) == 0;
                            }
                        }
                        if (on && tag_match) {
                            /* Scope guard (WHATWG): a foreign
                             * integration point between the
                             * current node and the match puts the
                             * target OUT OF SCOPE — the tag is
                             * ignored (13.2.4.2 boundary list). */
                            int fenced = 0;
                            if (b.whatwg &&
                                strcmp(lname, "template") != 0) {
                                for (size_t k = b.depth; k > d; k--)
                                    if (h_is_int_point(&b, k - 1)) {
                                        fenced = 1;
                                        break;
                                    }
                            }
                            if (fenced) break;
                            /* #659 "in template" fence: a
                             * non-template end tag whose nearest
                             * match is the template itself or an
                             * ancestor of it is ignored — nothing
                             * pops past the nearest template. */
                            if (b.whatwg) {
                                int ti = h_template_idx(&b);
                                if (ti >= 0 &&
                                    strcmp(lname, "template") != 0 &&
                                    (int)(d - 1) <= ti)
                                    break;
                            }
                            h_pop_to(&b, d - 1);
                            /* #659 content-level closes restore the
                             * template's saved mode (the reset-
                             * appropriately template clause):
                             * row -> in-table-body, cell -> in-row,
                             * section/caption/colgroup -> in-table. */
                            if (b.whatwg && b.depth > 0) {
                                const char* pt = leptris_element_name(
                                    b.open[b.depth - 1]);
                                if (pt &&
                                    strcmp(pt, "template") == 0) {
                                    unsigned char* tm =
                                        &b.tmpl_mode[b.depth - 1];
                                    if (strcmp(lname, "tr") == 0)
                                        *tm = H_TPLM_IN_TBODY;
                                    else if (strcmp(lname, "td") == 0 ||
                                             strcmp(lname, "th") == 0)
                                        *tm = H_TPLM_IN_ROW;
                                    else if (strcmp(lname, "tbody") == 0 ||
                                             strcmp(lname, "thead") == 0 ||
                                             strcmp(lname, "tfoot") == 0 ||
                                             strcmp(lname, "caption") == 0 ||
                                             strcmp(lname, "colgroup") == 0)
                                        *tm = H_TPLM_IN_TABLE;
                                }
                            }
                            /* Marker-scope closes clear the active
                             * formatting list up to their marker
                             * (13.2.4.3 cell/applet close). */
                            if (b.whatwg_adopt &&
                                (strcmp(on, "applet") == 0 ||
                                 strcmp(on, "marquee") == 0 ||
                                 strcmp(on, "object") == 0 ||
                                 strcmp(on, "td") == 0 ||
                                 strcmp(on, "th") == 0 ||
                                 strcmp(on, "caption") == 0 ||
                                 strcmp(on, "template") == 0))
                                h_afe_clear_to_marker(&b);
                            break;
                        }
                    }
                }
            }
            p = q;
            text = p;
            continue;
        }

        if (k1 == '?') {
            /* Processing-instruction-ish bogus construct (#659):
             * libxml2 keeps a PI node whose data INCLUDES the
             * trailing '?' — content runs to the first '>'. */
            if (text < p) {
                char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
                if (dec && *dec) {
                    LeptrisTextNode* t = leptris_text_create(
                        dec, strlen(dec), b.pool);
                    if (t) {
                        h_reconstruct(&b);
                        h_append(&b, (LeptrisNodeRef)t);
                    }
                }
            }
            const char* ts = p + 2;   /* skip "<?" */
            /* #659 WHATWG: html5lib has no PI tokenizer — "<?"
             * makes a bogus comment whose data is "?" plus the
             * raw bytes to the first '>' (tests1:40/41/44/47).
             * The html4 entry keeps the libxml2 PI node. */
            if (b.whatwg) {
                const char* q2 = ts;
                while (q2 < end && *q2 != '>') q2++;
                char* dat = (char*)leptris_pool_alloc(
                    b.pool, 1 + (size_t)(q2 - ts) + 1);
                if (dat) {
                    dat[0] = '?';
                    memcpy(dat + 1, ts, (size_t)(q2 - ts));
                    dat[1 + (q2 - ts)] = 0;
                    h_bogus_comment(&b, dat,
                                    1 + (size_t)(q2 - ts));
                }
                p = (q2 < end) ? q2 + 1 : end;
                text = p;
                continue;
            }
            const char* q = ts;
            while (q < end && !h_is_ws(*q) && *q != '>') q++;
            size_t tlen = (size_t)(q - ts);
            const char* de = (q < end && *q == '>') ? q : q;
            while (de < end && *de != '>') de++;
            const char* ds = ts + tlen;
            while (ds < de && h_is_ws(*ds)) ds++;   /* libxml2 trims */
            size_t dlen = (size_t)(de > ds ? de - ds : 0);
            char* tgt = (char*)leptris_pool_alloc(b.pool, tlen + 1);
            char* dat = (char*)leptris_pool_alloc(b.pool, dlen + 1);
            if (tgt && dat) {
                memcpy(tgt, ts, tlen);
                tgt[tlen] = 0;
                if (dlen) memcpy(dat, ds, dlen);
                dat[dlen] = 0;
                LeptrisNodeRef pi = leptris_pi_node_create(doc, tgt, dat);
                if (pi) h_append(&b, pi);
            }
            p = (de < end) ? de + 1 : end;
            text = p;
            continue;
        }

        if (!h_isalnum(k1) && k1 != '_' && k1 != ':') {
            /* '<' not starting a tag: literal text. */
            p++;
            continue;
        }

        /* Start tag. Even a dropped structural tag ends the
         * initial insertion mode — later comments are in-flow. */
        b.left_initial = 1;
        const char* ns = p + 1;
        const char* q = ns;
        while (q < end && !h_is_ws(*q) && *q != '>' && *q != '/') q++;
        size_t nlen = (size_t)(q - ns);
        char* name = h_pooled_lower(b.pool, ns, nlen);
        if (!name) goto done;

        /* Structural tags at top level (no explicit <html> open):
         * WHATWG's implicit head/body phases — the commit-time
         * synthesis provides the real elements, so the bare tags
         * themselves disappear. Inside an explicit <html> they are
         * ordinary elements (honored as-is). Their ATTRIBUTES are
         * stashed and land on the synthesized elements. */
        if (b.depth == 0 && !b.frameset &&
            (strcmp(name, "head") == 0 || strcmp(name, "body") == 0)) {
            if (strcmp(name, "body") == 0) {
                b.lift_closed = 1;
                if (!b.lift_boundary) b.lift_boundary = b.top_tail;
            }
            if (b.whatwg)
                h_stash_attrs(&b, q, end,
                              strcmp(name, "head") == 0
                                  ? b.head_attrs
                                  : b.body_attrs,
                              strcmp(name, "head") == 0
                                  ? &b.head_attr_n
                                  : &b.body_attr_n);
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            text = p;
            continue;
        }

        /* #659 frameset mode (WHATWG): a <frameset> before any
         * body content replaces the body; after content it is
         * ignored. Inside an open frameset it nests. html/head/
         * body tokens in frameset context are dropped. */
        if (b.whatwg && strcmp(name, "frameset") == 0) {
            if (!b.frameset && b.depth == 0 && h_body_still_empty(&b)) {
                b.frameset = 1;
                /* falls through: the normal open pushes it */
            } else if (!(b.frameset && b.depth > 0)) {
                /* Dropped token: flush pending text first. */
                if (text < p) {
                    char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
                    if (dec && *dec) {
                        LeptrisTextNode* t = leptris_text_create(
                            dec, strlen(dec), b.pool);
                        if (t) h_append(&b, (LeptrisNodeRef)t);
                    }
                }
                while (q < end && *q != '>') q++;
                p = (q < end) ? q + 1 : end;
                text = p;
                continue;
            }
        } else if (b.whatwg && b.frameset &&
                   (strcmp(name, "html") == 0 ||
                    strcmp(name, "head") == 0 ||
                    strcmp(name, "body") == 0)) {
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            text = p;
            continue;
        } else if (b.whatwg && h_template_idx(&b) >= 0 &&
                   (strcmp(name, "html") == 0 ||
                    strcmp(name, "head") == 0 ||
                    strcmp(name, "body") == 0)) {
            /* #659 "in template": structural tags drop entirely
             * (html5lib template.dat:64-67 — attrs do NOT merge
             * onto the outer elements). */
            if (text < p) {
                char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
                if (dec && *dec) {
                    LeptrisTextNode* t = leptris_text_create(
                        dec, strlen(dec), b.pool);
                    if (t) h_append(&b, (LeptrisNodeRef)t);
                }
            }
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            text = p;
            continue;
        }

        /* #659 "in frameset" (WHATWG 13.2.6.4.18): inside
         * frameset content only frameset/frame/noframes is live;
         * other start tags drop, non-whitespace text drops. */
        if (b.whatwg && b.frameset && b.depth > 0 &&
            strcmp(name, "frameset") != 0 &&
            strcmp(name, "frame") != 0 &&
            strcmp(name, "noframes") != 0) {
            if (text < p) {
                char* dec =
                    h_decode_ww(b.pool, text, p, 0, b.whatwg);
                if (dec && *dec) {
                    int ws = 1;
                    for (const char* q2 = dec; *q2; q2++)
                        if (!h_is_ws(*q2)) {
                            ws = 0;
                            break;
                        }
                    if (ws) {
                        LeptrisTextNode* t = leptris_text_create(
                            dec, strlen(dec), b.pool);
                        if (t)
                            h_append(&b, (LeptrisNodeRef)t);
                    }
                }
            }
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            text = p;
            continue;
        }

        /* Flush pending text before the element. */
        if (text < p) {
            char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
            if (dec && *dec) {
                LeptrisTextNode* t =
                    leptris_text_create(dec, strlen(dec), b.pool);
                if (t) {
                    h_reconstruct(&b);
                    h_append(&b, (LeptrisNodeRef)t);
                }
            }
        }

        /* #659 "in select" (WHATWG 13.2.6.4.7): only the select
         * set is live inside an open <select> — every other
         * start tag is dropped; its text content joins the
         * select's text. */
        if (b.whatwg && h_in_select(&b) &&
            strcmp(name, "option") != 0 &&
            strcmp(name, "optgroup") != 0 &&
            strcmp(name, "select") != 0 &&
            strcmp(name, "input") != 0 &&
            strcmp(name, "keygen") != 0 &&
            strcmp(name, "textarea") != 0 &&
            strcmp(name, "script") != 0 &&
            strcmp(name, "template") != 0) {
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            text = p;
            continue;
        }

        /* #659 "in head noscript" (scripting off): head content
         * stays inside the noscript; the first body-ish token
         * pops it and reprocesses at top level. */
        if (h_in_head_noscript(&b)) {
            if (strcmp(name, "html") == 0) {
                /* <html> inside head-noscript: attrs merge onto
                 * the html element; the tag is dropped. */
                h_stash_attrs(&b, q, end, b.html_attrs,
                              &b.html_attr_n);
                while (q < end && *q != '>') q++;
                p = (q < end) ? q + 1 : end;
                text = p;
                continue;
            }
            if (strcmp(name, "head") == 0 ||
                strcmp(name, "noscript") == 0) {
                while (q < end && *q != '>') q++;
                p = (q < end) ? q + 1 : end;
                text = p;
                continue;
            }
            int ns_ok = strcmp(name, "link") == 0 ||
                        strcmp(name, "meta") == 0 ||
                        strcmp(name, "style") == 0 ||
                        strcmp(name, "base") == 0 ||
                        strcmp(name, "basefont") == 0 ||
                        strcmp(name, "bgsound") == 0 ||
                        strcmp(name, "template") == 0 ||
                        strcmp(name, "script") == 0 ||
                        strcmp(name, "noframes") == 0;
            if (!ns_ok) b.depth--;   /* pop; reprocess below */
        }

        /* #659 foreign content (WHATWG 12.2.6.5). elem_ns is the
         * namespace the element this tag creates lands in. */
        int elem_ns = H_NS_HTML;
        if (b.whatwg) {
            /* An open <select> swallows foreign roots (in-select
             * ignores unknown start tags). */
            if ((strcmp(name, "svg") == 0 || strcmp(name, "math") == 0) &&
                h_in_select(&b)) {
                while (q < end && *q != '>') q++;
                p = (q < end) ? q + 1 : end;
                text = p;
                continue;
            }
            int sns = h_start_ns(&b, name);
            if (sns != H_NS_HTML) {
                if (h_is_breakout(name) ||
                    (strcmp(name, "font") == 0 && h_font_break(q, end))) {
                    /* Breakout: pop the foreign scope, reprocess
                     * under HTML rules below. */
                    while (b.depth > 0 &&
                           b.open_ns[b.depth - 1] != H_NS_HTML &&
                           !h_is_int_point(&b, b.depth - 1))
                        b.depth--;
                } else {
                    elem_ns = sns;
                }
            } else if (strcmp(name, "svg") == 0) {
                elem_ns = H_NS_SVG;
            } else if (strcmp(name, "math") == 0) {
                elem_ns = H_NS_MATH;
            }
        }

        /* Implied end tags this start tag triggers (HTML rules
         * only — foreign content has none). */
        if (elem_ns == H_NS_HTML) {
            /* WHATWG: block starts close an open p in BUTTON
             * SCOPE — formatting elements do not fence the scan,
             * they stay dangling in the active formatting list
             * and reconstruct inside the new block. */
            if (b.whatwg_adopt && h_p_closes(name)) {
                for (size_t d = b.depth; d > 0; d--) {
                    const char* on =
                        leptris_element_name(b.open[d - 1]);
                    if (!on) break;
                    if (strcmp(on, "p") == 0) {
                        b.depth = d - 1;
                        break;
                    }
                    if (h_ieq_raw(on, "button") ||
                        h_ieq_raw(on, "applet") ||
                        h_ieq_raw(on, "caption") ||
                        h_ieq_raw(on, "table") ||
                        h_ieq_raw(on, "td") ||
                        h_ieq_raw(on, "th") ||
                        h_ieq_raw(on, "marquee") ||
                        h_ieq_raw(on, "object") ||
                        h_ieq_raw(on, "select") ||
                        h_ieq_raw(on, "template") ||
                        h_is_int_point(&b, d - 1))
                        break;
                }
            }
            const char* tmpl_last_popped = NULL;
            while (b.depth > 0) {
                const char* on = leptris_element_name(b.open[b.depth - 1]);
                if (on &&
                    (h_closes(on, name) ||
                     (b.whatwg && h_closes_ww(on, name)))) {
                    /* #659 "in template" start-tag fence (13.2.4.2):
                     * an open template is a scope boundary — a start
                     * tag never pops it; table-context starts become
                     * template content instead (html5lib
                     * template.dat:28/32/36). */
                    if (b.whatwg && h_ieq_raw(on, "template"))
                        break;
                    tmpl_last_popped = on;
                    b.depth--;
                } else break;
            }
            /* #659: a section element this token just closed means
             * the new section/group token arrives in in-table mode
             * (gumbo in-table-body 3775: pop the open section,
             * switch to in-table, reprocess) — not a stray drop. */
            if (b.whatwg && tmpl_last_popped && b.depth > 0) {
                const char* tp =
                    leptris_element_name(b.open[b.depth - 1]);
                if (tp && h_ieq_raw(tp, "template") &&
                    (h_ieq_raw(tmpl_last_popped, "tbody") ||
                     h_ieq_raw(tmpl_last_popped, "thead") ||
                     h_ieq_raw(tmpl_last_popped, "tfoot") ||
                     h_ieq_raw(tmpl_last_popped, "caption") ||
                     h_ieq_raw(tmpl_last_popped, "colgroup")))
                    b.tmpl_mode[b.depth - 1] = H_TPLM_IN_TABLE;
            }
        }

        /* #659 template content-level table tokens: the per-
         * template insertion mode governs wrapping/dropping
         * (13.2.6.4.10 — see h_tmpl_content_start). Runs BEFORE the
         * generic in-table synthesis, which no-ops on a template
         * top for the bare/pass actions. */
        int tmpl_act = 5;
        if (b.whatwg && elem_ns == H_NS_HTML && b.depth > 0) {
            const char* ttop =
                leptris_element_name(b.open[b.depth - 1]);
            if (ttop && strcmp(ttop, "template") == 0) {
                tmpl_act = h_tmpl_content_start(&b, name);
            } else {
                /* #659 in-body stray drop (template.dat:57): a
                 * table-context token inside a template with NO
                 * table-family element between here and the template
                 * is body content — in-body rules ignore it. */
                int ttype = strcmp(name, "tr") == 0 ||
                            strcmp(name, "td") == 0 ||
                            strcmp(name, "th") == 0 ||
                            strcmp(name, "tbody") == 0 ||
                            strcmp(name, "thead") == 0 ||
                            strcmp(name, "tfoot") == 0 ||
                            strcmp(name, "caption") == 0 ||
                            strcmp(name, "colgroup") == 0 ||
                            strcmp(name, "col") == 0;
                if (ttype) {
                    int ti2 = h_template_idx(&b);
                    if (ti2 >= 0) {
                        int tableish = 0;
                        for (int k = (int)b.depth - 1; k > ti2; k--) {
                            const char* an = leptris_element_name(
                                b.open[k]);
                            if (an &&
                                (strcmp(an, "table") == 0 ||
                                 strcmp(an, "tbody") == 0 ||
                                 strcmp(an, "thead") == 0 ||
                                 strcmp(an, "tfoot") == 0 ||
                                 strcmp(an, "tr") == 0 ||
                                 strcmp(an, "td") == 0 ||
                                 strcmp(an, "th") == 0 ||
                                 strcmp(an, "caption") == 0 ||
                                 strcmp(an, "colgroup") == 0 ||
                                 strcmp(an, "col") == 0)) {
                                tableish = 1;
                                break;
                            }
                        }
                        if (!tableish) tmpl_act = 1;
                    }
                }
            }
        }
        if (tmpl_act == 1) {
            if (text < p) {
                char* dec = h_decode_ww(b.pool, text, p, 0, b.whatwg);
                if (dec && *dec) {
                    LeptrisTextNode* t = leptris_text_create(
                        dec, strlen(dec), b.pool);
                    if (t) h_append(&b, (LeptrisNodeRef)t);
                }
            }
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            text = p;
            continue;
        }
        if (tmpl_act == 2) {
            h_open_element(&b, "tr");
        } else if (tmpl_act == 3) {
            h_open_element(&b, "tbody");
        } else if (tmpl_act == 4) {
            h_open_element(&b, "tbody");
            h_open_element(&b, "tr");
        }

        /* #659 in-table wrapper synthesis (WHATWG 12.2.6.4, WHATWG
         * mode only): rows/cells/cols arriving directly under a
         * table get their tbody/tr/colgroup wrapper. libxml2/
         * Nokogiri keeps them bare — the html4 entry's parity
         * shape, unchanged here. */
        if (b.whatwg && elem_ns == H_NS_HTML && b.depth > 0) {
            /* Clear the stack back to a table context first: a
             * cell/row/group/caption start with stray elements
             * open ABOVE the table (a foster-parented <a>) pops
             * them before the wrapper synthesis runs. */
            {
                const char* topn =
                    leptris_element_name(b.open[b.depth - 1]);
                int tableish =
                    h_ieq_raw(topn, "table") ||
                    h_ieq_raw(topn, "tbody") ||
                    h_ieq_raw(topn, "thead") ||
                    h_ieq_raw(topn, "tfoot") ||
                    h_ieq_raw(topn, "tr") ||
                    h_ieq_raw(topn, "caption") ||
                    h_ieq_raw(topn, "colgroup");
                if (!tableish &&
                    (strcmp(name, "caption") == 0 ||
                     strcmp(name, "col") == 0 ||
                     strcmp(name, "colgroup") == 0 ||
                     strcmp(name, "tbody") == 0 ||
                     strcmp(name, "tfoot") == 0 ||
                     strcmp(name, "thead") == 0 ||
                     strcmp(name, "td") == 0 ||
                     strcmp(name, "th") == 0 ||
                     strcmp(name, "tr") == 0)) {
                    for (size_t d2 = b.depth; d2 > 0; d2--) {
                        const char* on2 =
                            leptris_element_name(b.open[d2 - 1]);
                        /* #659 fence: a template between here and the
                         * table owns the token — no clearing past it
                         * (template-top no-ops the synthesis below). */
                        if (on2 && h_ieq_raw(on2, "template"))
                            break;
                        if (on2 && h_ieq_raw(on2, "table")) {
                            b.depth = d2;
                            break;
                        }
                    }
                }
            }
            const char* tn = leptris_element_name(b.open[b.depth - 1]);
            int is_body = h_ieq_raw(tn, "tbody") ||
                          h_ieq_raw(tn, "thead") ||
                          h_ieq_raw(tn, "tfoot");
            if (h_ieq_raw(tn, "table")) {
                if (strcmp(name, "tr") == 0) {
                    h_open_element(&b, "tbody");
                } else if (strcmp(name, "td") == 0 ||
                           strcmp(name, "th") == 0) {
                    h_open_element(&b, "tbody");
                    h_open_element(&b, "tr");
                } else if (strcmp(name, "col") == 0) {
                    h_open_element(&b, "colgroup");
                }
            } else if (is_body &&
                       (strcmp(name, "td") == 0 ||
                        strcmp(name, "th") == 0)) {
                h_open_element(&b, "tr");
            }
        }

        /* #659 a/nobr start tags run the adoption agency first
         * when an open element of the same name is still in the
         * active formatting list (13.2.6.4.7) — the duplicate
         * closes before the new one opens. */
        if (b.whatwg_adopt && elem_ns == H_NS_HTML &&
            (strcmp(name, "a") == 0 || strcmp(name, "nobr") == 0)) {
            if (h_afe_find(&b, name) >= 0) {
                h_afe_end(&b, name);
                /* The agency's 8-iteration cap can leave the
                 * entry — the start-tag branch removes it
                 * unconditionally. */
                int ai = h_afe_find(&b, name);
                if (ai >= 0) {
                    LeptrisElement fe = b.afe[ai];
                    h_afe_remove_idx(&b, ai);
                    int ri = h_stack_find(&b, fe);
                    if (ri >= 0) {
                        memmove(&b.open[ri], &b.open[ri + 1],
                                (b.depth - ri - 1) *
                                    sizeof(b.open[0]));
                        memmove(&b.open_ns[ri], &b.open_ns[ri + 1],
                                (b.depth - ri - 1));
                        b.depth--;
                    }
                }
            }
        }
        /* #659 reconstruct the active formatting elements before
         * this insertion (13.2.6.4.7 reconstructs on character
         * tokens and on formatting/ordinary element starts — NOT
         * on the structural/head/block/table set). */
        if (b.whatwg_adopt && elem_ns == H_NS_HTML &&
            h_reconstructs(name))
            h_reconstruct(&b);

        LeptrisElement e = (elem_ns != H_NS_HTML)
                               ? h_open_foreign(&b, name, elem_ns)
                               : h_open_element(&b, name);
        if (!e) goto done;
        if (b.whatwg && elem_ns == H_NS_HTML &&
            strcmp(name, "template") == 0 && b.depth > 0) {
            b.tmpl_mode[b.depth - 1] = H_TPLM_TEMPLATE;
        }

        /* Attributes. */
        int self_closing = 0;
        for (;;) {
            while (q < end && h_is_ws(*q)) q++;
            if (q >= end) break;
            if (*q == '>') { q++; break; }
            if (*q == '/') {
                /* '/' then '>' = self-closing; a bare '/' is
                 * ignored (HTML does not use it otherwise). */
                if (q + 1 < end && q[1] == '>') {
                    self_closing = 1;
                    q += 2;
                    break;
                }
                q++;
                continue;
            }
            const char* as = q;
            while (q < end && !h_is_ws(*q) && *q != '=' && *q != '>' &&
                   *q != '/')
                q++;
            size_t alen = (size_t)(q - as);
            if (!alen) { q++; continue; }
            char* aname = h_pooled_lower(b.pool, as, alen);
            if (!aname) goto done;
            const char* vs = NULL;
            size_t vlen = 0;
            const char* scan = q;
            while (scan < end && h_is_ws(*scan)) scan++;
            if (scan < end && *scan == '=') {
                scan++;
                while (scan < end && h_is_ws(*scan)) scan++;
                if (scan < end && (*scan == '\'' || *scan == '"')) {
                    char quote = *scan++;
                    vs = scan;
                    while (scan < end && *scan != quote) scan++;
                    vlen = (size_t)(scan - vs);
                    if (scan < end) scan++;
                } else {
                    vs = scan;
                    while (scan < end && !h_is_ws(*scan) && *scan != '>')
                        scan++;
                    vlen = (size_t)(scan - vs);
                }
                q = scan;
            } else {
                /* Minimized attribute: value = name (checked). */
                vs = NULL;
            }
            char* aval;
            if (vs) {
                aval = h_decode_ex(b.pool, vs, vs + vlen, 1,
                                   b.whatwg);
            } else {
                /* Minimized (boolean) attribute: value is the EMPTY
                 * string (html5lib/Nokogiri DOM: checked=""). */
                aval = (char*)"";
            }
            if (aval) {
                /* #659: foreign attribute-name adjustment
                 * (viewBox, definitionURL, ...). */
                const char* aadj =
                    elem_ns != H_NS_HTML ? h_attr_name(elem_ns, aname)
                                         : NULL;
                leptris_element_add_attribute(
                    e, leptris_sv_from_cstr(aadj ? aadj : aname),
                    leptris_sv_from_cstr(aval), b.pool);
            }
        }

        /* Raw-text elements consume until their close tag (HTML
         * tokenizer switch only — foreign <script>/<style> are
         * ordinary foreign elements). #659: <plaintext> is
         * raw-to-EOF (WHATWG — it has no close form). */
        int raw_name = h_is_raw(name) ||
                       (b.whatwg &&
                        (strcmp(name, "plaintext") == 0 ||
                         strcmp(name, "noframes") == 0 ||
                         strcmp(name, "title") == 0 ||
                         strcmp(name, "textarea") == 0 ||
                         strcmp(name, "iframe") == 0 ||
                         strcmp(name, "noembed") == 0 ||
                         strcmp(name, "xmp") == 0));
        if (raw_name && !self_closing && elem_ns == H_NS_HTML) {
            const char* rs = q;
            if (strcmp(name, "plaintext") == 0) {
                rs = end;   /* eats the rest of the input */
            } else if (b.whatwg && strcmp(name, "script") == 0) {
                /* #659 script-data escaped states (13.2.5.15-.31,
                 * html5lib tests16:38-48/64-72): "<!--" enters
                 * script-data-escaped; there "</script" + delimiter
                 * closes while "<script" + delimiter enters
                 * double-escaped (one </script> only drops back);
                 * "-->"/"--!>" re-enter plain script data. */
                int esc = 0, dbl = 0;
                while (rs < end) {
                    if (rs[0] == '<') {
                        int is_end = rs + 1 < end && rs[1] == '/';
                        int is_open = rs + 1 < end && rs[1] != '/';
                        const char* tn = rs + (is_end ? 2 : 1);
                        if (tn + 6 <= end) {
                            static const char kw[] = "script";
                            int m = 1;
                            for (int i = 0; i < 6; i++)
                                if (h_lower(tn[i]) != kw[i]) {
                                    m = 0;
                                    break;
                                }
                            char d = tn[6];
                            if (m && (d == '>' || d == '/' || d == ' ' ||
                                      d == '\t' || d == '\n' ||
                                      d == '\r' || d == '\f')) {
                                if (is_end) {
                                    if (dbl) {
                                        dbl = 0;   /* one level back */
                                    } else {
                                        break;      /* close here */
                                    }
                                } else if (esc) {
                                    dbl = 1;
                                }
                                rs = tn + 6;
                                continue;
                            }
                        }
                        if (rs + 4 <= end && rs[1] == '!' &&
                            rs[2] == '-' && rs[3] == '-') {
                            if (!esc && !dbl) esc = 1;
                            rs += 4;
                            continue;
                        }
                        rs++;
                        continue;
                    }
                    if ((esc || dbl) && rs[0] == '-' && rs + 3 <= end &&
                        ((rs[1] == '-' && rs[2] == '>') ||
                         (rs[1] == '-' && rs[2] == '!' &&
                          rs + 4 <= end && rs[3] == '>'))) {
                        /* --> or --!>: drop out of the escaped
                         * states entirely. */
                        esc = 0;
                        dbl = 0;
                        rs += (rs[2] == '!') ? 4 : 3;
                        continue;
                    }
                    rs++;
                }
            } else {
                while (rs < end) {
                    if (rs + 2 + nlen + 1 <= end && rs[0] == '<' &&
                        rs[1] == '/') {
                        size_t i = 0;
                        for (; i < nlen; i++)
                            if (h_lower(rs[2 + i]) != name[i]) break;
                        if (i == nlen) break;
                    }
                    rs++;
                }
            }
            if (rs > q) {
                /* RCDATA (title/textarea) decodes entities and
                 * textarea drops one leading newline (13.2.6.2
                 * authoring convenience); the rest is raw. */
                const char* cs = q;
                size_t clen = (size_t)(rs - q);
                if (b.whatwg &&
                    (strcmp(name, "title") == 0 ||
                     strcmp(name, "textarea") == 0)) {
                    if (strcmp(name, "textarea") == 0 && *cs == '\n') {
                        cs++;
                        clen--;
                    }
                    char* dec =
                        h_decode_ww(b.pool, cs, cs + clen, 0, b.whatwg);
                    if (dec) {
                        LeptrisTextNode* t = leptris_text_create(
                            dec, strlen(dec), b.pool);
                        if (t)
                            leptris_element_append_child_internal_doc(
                                e, (LeptrisNodeRef)t, b.doc);
                    }
                } else {
                    LeptrisTextNode* t = leptris_text_create(
                        cs, clen, b.pool);
                    if (t)
                        leptris_element_append_child_internal_doc(
                            e, (LeptrisNodeRef)t, b.doc);
                }
            }
            /* Skip past the close tag. */
            const char* cq = rs;
            while (cq < end && *cq != '>') cq++;
            p = (cq < end) ? cq + 1 : end;
            b.depth--;   /* the raw element is complete */
            text = p;
            continue;
        }

        /* #659 active formatting list: formatting elements push
         * their entry; applet/object/marquee/td/th/caption open a
         * marker scope (13.2.4.3). */
        if (b.whatwg_adopt && elem_ns == H_NS_HTML &&
            !(self_closing || h_is_void(name))) {
            if (h_is_formatting(name)) {
                h_afe_push(&b, e);
            } else if (strcmp(name, "applet") == 0 ||
                       strcmp(name, "marquee") == 0 ||
                       strcmp(name, "object") == 0 ||
                       strcmp(name, "td") == 0 ||
                       strcmp(name, "th") == 0 ||
                       strcmp(name, "caption") == 0) {
                h_afe_marker_push(&b);
            }
        }
        if (self_closing || h_is_void(name)) b.depth--;
        p = q;
        text = p;
    }

    /* Trailing text. */
    if (text < end) {
        char* dec = h_decode_ww(b.pool, text, end, 0, b.whatwg);
        if (dec && *dec) {
            LeptrisTextNode* t =
                leptris_text_create(dec, strlen(dec), b.pool);
            if (t) {
                h_reconstruct(&b);
                h_append(&b, (LeptrisNodeRef)t);
            }
        }
    }

done:
    ;
    /* Nothing appended (empty input, stray end tags only, doctype
     * only) is NOT an error in lenient HTML mode: the wrapper
     * synthesis below yields the empty Nokogiri shape. */
    /* Nokogiri document shape (#659): with no explicit <html>,
     * synthesize <html><head></head><body>content</body></html> —
     * the document model is single-rooted, and libxml2's
     * htmlParseDocument does the same. An input <html> element is
     * honored as-is (head/body inside it stay untouched). */
    int has_html = 0;
    for (LeptrisNodeRef c = b.top_head; c;
         c = leptris_node_get_next_sibling(c)) {
        if (leptris_node_get_type(c) == LEPTRIS_NODE_TYPE_ELEMENT &&
            h_ieq_raw(leptris_element_name((LeptrisElement)c), "html")) {
            has_html = 1;
            break;
        }
    }
    if (!has_html) {
        /* EOF closes every open element (never fails the parse),
         * and the parsed chain moves under a fresh <html>. */
        b.depth = 0;
        LeptrisNodeRef orig_head = b.top_head;
        b.top_head = NULL;
        b.top_tail = NULL;
        LeptrisElement html = h_open_named(&b, "html", 0);
        if (!html) {
            if (status) *status = LEPTRIS_ERROR_MEMORY;
            leptris_document_free(doc);
            return NULL;
        }
        h_split_head_body(&b, html, orig_head);
        b.top_head = (LeptrisNodeRef)html;
        b.top_tail = (LeptrisNodeRef)html;
        b.root = html;
    } else {
        /* Explicit <html>: it is the root; keep top-level order
         * (prolog comments before it stay in the chain). */
        for (LeptrisNodeRef c = b.top_head; c;
             c = leptris_node_get_next_sibling(c)) {
            if (leptris_node_get_type(c) == LEPTRIS_NODE_TYPE_ELEMENT) {
                b.root = (LeptrisElement)c;
                break;
            }
        }
        /* #659 WHATWG: the same head/body split applies inside an
         * explicit <html> — lift the leading head run, wrap the
         * rest (an explicit <head>/<body> child keeps the rest in
         * place; the ensure step below adds what is missing).
         * libxml2 keeps explicit-html children untouched. */
        if (b.whatwg_head_set && b.root &&
            h_ieq_raw(leptris_element_name(b.root), "html") &&
            leptris_node_first_child((LeptrisNodeRef)b.root)) {
            h_split_head_body(&b, b.root,
                              leptris_node_first_child(
                                  (LeptrisNodeRef)b.root));
        }
    }

    /* #659 WHATWG: every document is html>[head, body] — head
     * possibly empty — whatever the bare structural tags looked
     * like. The html4/libxml2 mode keeps its shape (no empty
     * head/body; Nokogiri's <html></html>). */
    if (b.whatwg_head_set && b.root &&
        h_ieq_raw(leptris_element_name(b.root), "html")) {
        int has_head = 0, has_body = 0;
        for (LeptrisNodeRef c =
                 leptris_node_first_child((LeptrisNodeRef)b.root);
             c; c = leptris_node_get_next_sibling(c)) {
            if (leptris_node_get_type(c) != LEPTRIS_NODE_TYPE_ELEMENT)
                continue;
            const char* cn = leptris_element_name((LeptrisElement)c);
            if (h_ieq_raw(cn, "head"))
                has_head = 1;
            else if (h_ieq_raw(cn, "body") || h_ieq_raw(cn, "frameset"))
                has_body = 1;
        }
        if (!has_body)
            h_new_child(&b, b.root, b.frameset ? "frameset" : "body");
        if (!has_head) {
            /* Create WITHOUT attaching (h_open_named appends to the
             * top chain) — head splices in as the FIRST child. */
            LeptrisStringView nv = leptris_sv_from_cstr("head");
            LeptrisElement head = leptris_element_create_with_view(nv, b.pool);
            if (head) {
                leptris_root_doc_register(head, b.doc);
                LeptrisNodeRef first =
                    leptris_node_first_child((LeptrisNodeRef)b.root);
                if (first)
                    leptris_node_set_next_sibling((LeptrisNodeRef)head,
                                                  first);
                else
                    leptris_elem_set_last_child(b.root, (LeptrisNodeRef)head);
                leptris_elem_set_first_child(b.root, (LeptrisNodeRef)head);
                leptris_element_set_parent(head, b.root);
                b.root->child_count++;
            }
        }
    }
    /* #659: the dropped structural <head>/<body> tags' attrs land
     * on the synthesized elements; <html> attrs (from a structural
     * or head-noscript <html> token) land on the root. */
    if (b.whatwg && b.root &&
        h_ieq_raw(leptris_element_name(b.root), "html")) {
        if (b.html_attr_n)
            h_apply_attrs(&b, b.root, b.html_attrs, b.html_attr_n);
        for (LeptrisNodeRef c =
                 leptris_node_first_child((LeptrisNodeRef)b.root);
             c; c = leptris_node_get_next_sibling(c)) {
            if (leptris_node_get_type(c) != LEPTRIS_NODE_TYPE_ELEMENT)
                continue;
            const char* cn = leptris_element_name((LeptrisElement)c);
            if (b.head_attr_n && h_ieq_raw(cn, "head"))
                h_apply_attrs(&b, (LeptrisElement)c, b.head_attrs,
                              b.head_attr_n);
            else if (b.body_attr_n &&
                     (h_ieq_raw(cn, "body") || h_ieq_raw(cn, "frameset")))
                h_apply_attrs(&b, (LeptrisElement)c, b.body_attrs,
                              b.body_attr_n);
        }
    }
    doc->new_dom_root = b.root;
    leptris_root_doc_register(b.root, doc);
    if (b.prolog_head) {
        leptris_node_set_next_sibling(b.prolog_tail,
                                      (LeptrisNodeRef)b.top_head);
        doc->doc_children_head = b.prolog_head;
    } else {
        doc->doc_children_head = (LeptrisNodeRef)b.top_head;
    }
    doc->doc_children_tail = b.top_tail;
    return doc;
}

LEPTRIS_API LeptrisDocument leptris_parse_html_string(
    const char* buf, size_t len, LeptrisStatus* status) {
    /* #659: THE WHATWG ENGINE — the full "in head" set lifts into
     * the implied head (script/style/noscript/template/...). The
     * html5lib corpus is this entry's conformance meter. */
    return html_parse_shared(buf, len, status, 1);
}

LEPTRIS_API LeptrisDocument leptris_parse_html4_string(
    const char* buf, size_t len, LeptrisStatus* status) {
    /* #659: libxml2/Nokogiri compatibility — leading script/style
     * stay in body (title/meta/link/base still lift). The
     * committed Nokogiri reference trees measure this entry. */
    return html_parse_shared(buf, len, status, 0);
}
