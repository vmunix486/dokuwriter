// =============================================================================
// DokuWriter.cpp
// A simple single-file WYSIWYG word processor for Windows XP+
// Compiled with Microsoft Visual C++ 2008 (or later)
//
// File format: DokuWiki-style plain text markup
//   **bold**        -> bold text
//   //italic//      -> italic text
//   __underline__   -> underlined text
//   \n\n            -> paragraph break (blank line)
//   ====== Heading 1 ======
//   ===== Heading 2 =====
//   ==== Heading 3 ====
//   * item          -> bullet list item (line starts with "  * ")
//
// Build (MSVC 2008 command line):
//   cl DokuWriter.cpp /link user32.lib gdi32.lib comctl32.lib comdlg32.lib
//
// Or create a new "Win32 Project" (not console) in MSVC 2008, replace all
// generated .cpp content with this file, and build.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0501   // Target Windows XP
#define WINVER       0x0501

#include <windows.h>
#include <commdlg.h>          // GetOpenFileName / GetSaveFileName
#include <richedit.h>         // RichEdit control constants
#include <string>
#include <vector>
#include <sstream>

// =============================================================================
// CONSTANTS & IDs
// =============================================================================

#define IDC_RICHEDIT        1
#define IDM_FILE_NEW        101
#define IDM_FILE_OPEN       102
#define IDM_FILE_SAVE       103
#define IDM_FILE_SAVEAS     104
#define IDM_FILE_EXIT       105
#define IDM_FORMAT_BOLD     201
#define IDM_FORMAT_ITALIC   202
#define IDM_FORMAT_UNDER    203
#define IDM_FORMAT_H1       204
#define IDM_FORMAT_H2       205
#define IDM_FORMAT_H3       206
#define IDM_FORMAT_BULLET   207
#define IDM_HELP_ABOUT      301
#define IDM_VIEW_HIDEMARKUP 401
#define LINK_COLOR			RGB(0, 102, 204)
#define IDM_VIEW_WORDWRAP	402
#define IDM_VIEW_ZOOMIN		403
#define IDM_VIEW_ZOOMOUT	404 // Error: not found					rofl

// Toolbar button IDs (same as menu IDs for simplicity)
#define ID_TB_NEW           IDM_FILE_NEW
#define ID_TB_OPEN          IDM_FILE_OPEN
#define ID_TB_SAVE          IDM_FILE_SAVE
#define ID_TB_BOLD          IDM_FORMAT_BOLD
#define ID_TB_ITALIC        IDM_FORMAT_ITALIC
#define ID_TB_UNDER         IDM_FORMAT_UNDER

// Twips per point (RichEdit uses twips: 1 point = 20 twips)
#define TWIPS(pt) ((pt) * 20)

// How many milliseconds after typing before we reformat
#define REFORMAT_DELAY_MS   300

// Timer ID for deferred reformatting
#define IDT_REFORMAT        1

// =============================================================================
// GLOBALS
// =============================================================================

static HINSTANCE g_hInst        = NULL;
static HWND      g_hMainWnd     = NULL;
static HWND      g_hEdit        = NULL;
static HWND      g_hToolbar     = NULL;

static std::string g_currentFile = "";   // Current open file path (empty = untitled)
static bool        g_modified    = false; // Has the document been modified?
static bool        g_reformatting = false; // Guard against recursive EN_CHANGE
static bool		   g_hideMarkup  = false; // false = show markup tokens, true = hide them
static bool	       g_wordWrap	= false; // false = no word wrapping, true = word wrapping
static int		   g_baseFontSize = 11; // points; scales up/down with zoom

// =============================================================================
// MARKUP PARSER
// Helper functions that apply DokuWiki-style formatting to the RichEdit control.
// Strategy: after a short idle delay we scan the raw text line by line,
// reset all formatting, then re-apply character formatting for each markup span.
// =============================================================================

// --------------------------------------------------------------------------
// SetCharFmt: Apply a CHARFORMAT2 to a specific character range [start, end)
// --------------------------------------------------------------------------
static void SetCharFmt(int start, int end, CHARFORMAT2 &cf)
{
    CHARRANGE cr;
    cr.cpMin = start;
    cr.cpMax = end;
    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessage(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

// --------------------------------------------------------------------------
// MakeCF2: Build a CHARFORMAT2 with common defaults, then let caller adjust
// --------------------------------------------------------------------------
static CHARFORMAT2 MakeCF2()
{
    CHARFORMAT2 cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    return cf;
}

// --------------------------------------------------------------------------
// ApplyBold: Apply bold to [start, end)
// --------------------------------------------------------------------------
static void ApplyBold(int start, int end)
{
    CHARFORMAT2 cf = MakeCF2();
    cf.dwMask    = CFM_BOLD;
    cf.dwEffects = CFE_BOLD;
    SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ApplyItalic
// --------------------------------------------------------------------------
static void ApplyItalic(int start, int end)
{
    CHARFORMAT2 cf = MakeCF2();
    cf.dwMask    = CFM_ITALIC;
    cf.dwEffects = CFE_ITALIC;
    SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ApplyUnderline
// --------------------------------------------------------------------------
static void ApplyUnderline(int start, int end)
{
    CHARFORMAT2 cf = MakeCF2();
    cf.dwMask    = CFM_UNDERLINE;
    cf.dwEffects = CFE_UNDERLINE;
    SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ApplyFontSize: Set font size (in points) for [start, end)
// --------------------------------------------------------------------------
static void ApplyFontSize(int start, int end, int points)
{
    CHARFORMAT2 cf = MakeCF2();
    cf.dwMask = CFM_SIZE | CFM_BOLD;
    cf.yHeight = TWIPS(points);
    cf.dwEffects = CFE_BOLD; // Headings are always bold
    SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ApplyColor: Set foreground color for [start, end)
// --------------------------------------------------------------------------
static void ApplyColor(int start, int end, COLORREF color)
{
    CHARFORMAT2 cf = MakeCF2();
    cf.dwMask      = CFM_COLOR;
    cf.crTextColor = color;
    cf.dwEffects   = 0; // Must clear CFE_AUTOCOLOR
    SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ApplyHidden: Completely hide the markup formatting text
// --------------------------------------------------------------------------
static void ApplyHidden(int start, int end, bool hidden)
{
	CHARFORMAT2 cf = MakeCF2();
	cf.dwMask = CFM_HIDDEN;
	cf.dwEffects = hidden ? CFE_HIDDEN : 0;
	SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ApplyBackground: Highlighting (changing the color of the background behind
// the text
// --------------------------------------------------------------------------
static void ApplyBackground(int start, int end, COLORREF color)
{
	CHARFORMAT2A cf = MakeCF2();
	cf.dwMask = CFM_BACKCOLOR;
	cf.crBackColor = color;
	SetCharFmt(start, end, cf);
}

// --------------------------------------------------------------------------
// ParseLineCitation: Parse citations
// --------------------------------------------------------------------------
static void ParseLineCitation(const std::string &line, int lineOffset, int lineLen)
{
	// Citation block: ((text))
	// Must start with (( and end with ))
	if (line.size() < 5) return;
	if (line.substr(0,2) != "((" ) return;
	size_t close = line.rfind("))");
	if (close == std::string::npos || close < 2) return;

	int contentStart = lineOffset + 2;
	int contentEnd = (int)(lineOffset + close);
	int tokenEnd = lineOffset + lineLen;

	// Style the (( prefix
	ApplyColor(lineOffset, lineOffset + 2, RGB(180,180,180));
	ApplyHidden(lineOffset, lineOffset + 2, g_hideMarkup);

	// Style the content: italic, muted grey
	if (contentEnd > contentStart)
	{
		CHARFORMAT2A cf = MakeCF2();
		cf.dwMask = CFM_ITALIC | CFM_COLOR;
		cf.dwEffects = CFE_ITALIC;
		cf.crTextColor = RGB(100,100,100);
		SetCharFmt(contentStart, contentEnd, (CHARFORMAT2A &)cf);
	}

	// Style the )) suffix
	ApplyColor((int)(lineOffset + close), (int)(lineOffset + close + 2), RGB(180,180,180));
	ApplyHidden((int)(lineOffset + close), (int)(lineOffset + close + 2), g_hideMarkup);

	// Indent the whole line via paragraph formatting
	CHARRANGE cr;
	cr.cpMin = lineOffset;
	cr.cpMax = lineOffset + lineLen;
	SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);

	PARAFORMAT2 pf;
	ZeroMemory(&pf, sizeof(pf));
	pf.cbSize = sizeof(pf);
	pf.dwMask = PFM_STARTINDENT | PFM_RIGHTINDENT | PFM_BORDER;
	pf.dxStartIndent = 720; // ~0.5 inch left indent 
	pf.dxStartIndent = 720; // ~0.5 inch right indent
	pf.wBorders = 0x001; // left border only
	pf.wBorderWidth = 15; // border thickness in twips
	SendMessage(g_hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

// --------------------------------------------------------------------------
// ParseLineSpans:
//   Scans a single line (given as a std::string) for inline markup tokens
//   (**bold**, //italic//, __underline__) and applies formatting.
//   lineOffset = character offset of the first character of this line in the
//   overall RichEdit document.
// --------------------------------------------------------------------------
static void ParseLineSpans(const std::string &line, int lineOffset)
{
    // Tokens: delimiter -> effect function
    struct Token {
        const char *delim;
        int         dlen;
        void (*apply)(int, int);
    };

    Token tokens[] = {
        { "**", 2, ApplyBold      },
        { "//", 2, ApplyItalic    },
        { "__", 2, ApplyUnderline },
    };
    const int numTokens = 3;

    for (int t = 0; t < numTokens; ++t)
    {
        const char *delim = tokens[t].delim;
        int dlen = tokens[t].dlen;

        size_t pos = 0;
        while (pos < line.size())
        {
            size_t open = line.find(delim, pos);
            if (open == std::string::npos) break;

            size_t close = line.find(delim, open + dlen);
            if (close == std::string::npos) break;

            // Apply formatting to the content between the delimiters.
            // We also want to visually dim the delimiter characters themselves.
            int contentStart = (int)(lineOffset + open + dlen);
            int contentEnd   = (int)(lineOffset + close);

            if (contentEnd > contentStart)
                tokens[t].apply(contentStart, contentEnd);

            // Grey out the delimiter tokens so they are visible but unobtrusive
            ApplyColor((int)(lineOffset + open),  (int)(lineOffset + open + dlen),  RGB(180,180,180));
            ApplyColor((int)(lineOffset + close), (int)(lineOffset + close + dlen), RGB(180,180,180));

            pos = close + dlen;
        }
    }
	// Monospace
	{
		const char *delim = "''";
		const int dlen = 2;
		size_t pos = 0;
		while (pos < line.size())
		{
			size_t open = line.find(delim, pos);
			if (open == std::string::npos) break;

			size_t close = line.find(delim, open + dlen);
			if (close == std::string::npos) break;

			int contentStart = (int)(lineOffset + open + dlen);
			int contentEnd = (int)(lineOffset + close);

			if (contentEnd > contentStart)
			{
				// Red monospaced text with grey background
				CHARFORMAT2A cf = MakeCF2();
				cf.dwMask = CFM_FACE | CFM_COLOR | CFM_BACKCOLOR;
				cf.dwEffects = 0;
				cf.crTextColor = RGB(180,0,0);
				cf.crBackColor = RGB(220,220,220);
				lstrcpyA((char*)cf.szFaceName, "Courier New");
				SetCharFmt(contentStart, contentEnd, (CHARFORMAT2A &)cf);
			}

			// Grey out and optionally hide the '' delimiters
			ApplyColor((int)(lineOffset + open), (int)(lineOffset + open + dlen), RGB(180,180,180));
			ApplyHidden((int)(lineOffset + open), (int)(lineOffset + open + dlen), g_hideMarkup);
			ApplyColor((int)(lineOffset + close), (int)(lineOffset + close + dlen), RGB(180,180,180));
			ApplyHidden((int)(lineOffset + close), (int)(lineOffset + close + dlen), g_hideMarkup);

			pos = close + dlen;
		}
	}
}

// --------------------------------------------------------------------------
// ParseLineLinks: Add pseudo support for adding links (so my wiki start page
// looks better
// --------------------------------------------------------------------------
static void ParseLineLinks(const std::string &line, int lineOffset)
{
	size_t pos = 0;
	while (pos < line.size())
	{
		// Find opening [[
		size_t open = line.find("[[", pos);
		if (open == std::string::npos) break;

		// Find closing ]]
		size_t close = line.find("]]", open + 2);
		if (close == std::string::npos) break;

		// Everything between [[ and ]]
		std::string inner = line.substr(open + 2, close - open - 2);

		// Split on | to file URL and label
		size_t pipe = inner.find('|');
		std::string label;
		if (pipe != std::string::npos)
			label = inner.substr(pipe + 1); // text after the pipe
		else
			label = inner;  // no pipe: the whole thing is both URL and label

		// Character ranges in the document
		int tokenStart = (int)(lineOffset + open);		// Start of [[
		int labelStart = (int)(lineOffset + open + 2);	// start of content
		int labelEnd = (int)(lineOffset + close);		// end of content (before ]])
		int tokenEnd = (int)(lineOffset + close + 2);	// end of ]]

		if (pipe != std::string::npos)
		{
			// Hide or grey the URL portion and the pipe: [[ url | label ]]
			int urlEnd = (int)(lineOffset + open + 2 + (int)pipe + 1); // up to and including |

			// Style the [[ prefix
			ApplyColor(tokenStart, labelStart, RGB(180,180,180));
			ApplyHidden(tokenStart, labelStart, g_hideMarkup);

			// Style "url|" - grey/hidden
			ApplyColor(labelStart, urlEnd, RGB(180,180,180));
			ApplyHidden(labelStart, urlEnd, g_hideMarkup);

			// Style the label text - bold, underlined, and blue
			int displayStart = urlEnd;
			int displayEnd = labelEnd;
			if (displayEnd > displayStart)
			{
				CHARFORMAT cf = MakeCF2();
				cf.dwMask = CFM_BOLD | CFM_UNDERLINE | CFM_COLOR;
				cf.dwEffects = CFE_BOLD | CFE_UNDERLINE;
				cf.crTextColor = LINK_COLOR;
				SetCharFmt(displayStart, displayEnd, (CHARFORMAT2 &)cf);

				ApplyColor(labelEnd, tokenEnd, RGB(180, 180, 180));
				ApplyHidden(labelEnd, tokenEnd, g_hideMarkup);
			}

			pos = close + 2;
		}
	}
}

// --------------------------------------------------------------------------
// ReapplyFormatting:
//   Called after every typing pause. Resets all formatting to a plain default,
//   then walks each line and applies markup-driven formatting.
// --------------------------------------------------------------------------
static void ReapplyFormatting()
{
    if (!g_hEdit) return;

    // --- 1. Save caret / selection position ---
    CHARRANGE savedSel;
    SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&savedSel);

    // --- 2. Freeze redraws to prevent flicker ---
    SendMessage(g_hEdit, WM_SETREDRAW, FALSE, 0);

    // --- 3. Select all and reset to default character formatting ---
    {
        CHARRANGE all = {0, -1};
        SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&all);

        CHARFORMAT2 cfDefault = MakeCF2();
        cfDefault.dwMask      = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE |
                                CFM_SIZE | CFM_COLOR   | CFM_FACE |
								CFM_BACKCOLOR;
		cfDefault.crBackColor = RGB(255,255,255);
        cfDefault.dwEffects   = 0;
        cfDefault.yHeight     = TWIPS(g_baseFontSize); // Text size
        cfDefault.crTextColor = RGB(0, 0, 0);
        lstrcpyA(cfDefault.szFaceName, "Times New Roman");
        SendMessage(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfDefault);
    }

    // --- 4. Get the full plain text ---
    int textLen = GetWindowTextLength(g_hEdit);
    if (textLen == 0)
    {
        // Restore and unfreeze
        SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&savedSel);
        SendMessage(g_hEdit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hEdit, NULL, TRUE);
        return;
    }

    std::string text(textLen + 1, '\0');
    GetWindowTextA(g_hEdit, &text[0], textLen + 1);
    text.resize(textLen);

    // --- 5. Walk lines ---
    // RichEdit uses '\r' as its line separator internally (not '\n').
    // GetWindowText returns '\r\n' on some versions; we normalise to '\r'.
    // We iterate by finding '\r' boundaries.

    int lineOffset = 0; // Character offset of current line's start in RichEdit

    size_t pos = 0;
    while (pos <= text.size())
    {
        // Find end of this line
        size_t eol = text.find('\r', pos);
        if (eol == std::string::npos) eol = text.size();

        std::string line = text.substr(pos, eol - pos);
        int lineLen = (int)line.size();

        // -- Heading detection --
        // DokuWiki: ====== H1 ======  (6 = signs), ===== H2 =====, ==== H3 ====
        // We detect by counting leading '=' characters.
        if (line.size() >= 6 && line[0] == '=')
        {
            int eqCount = 0;
            while (eqCount < (int)line.size() && line[eqCount] == '=') eqCount++;

            // Trailing '=' should mirror the leading ones (DokuWiki style).
            // We just need at least 4 to treat as a heading.
            if (eqCount >= 4)
            {
                int pts = (eqCount >= 6) ? 22 :
                          (eqCount >= 5) ? 18 : 15;
                ApplyFontSize(lineOffset, lineOffset + lineLen, pts);
                ApplyColor(lineOffset, lineOffset + lineLen, RGB(0, 70, 140));
                // Grey out the = signs at each end, and if hidden, then hide it
				ApplyHidden(lineOffset, lineOffset + eqCount, g_hideMarkup);
                ApplyColor(lineOffset, lineOffset + eqCount, RGB(180,180,180));
                int trailStart = lineOffset;
                // find last non-space, non-'=' position
                size_t rpos = line.find_last_not_of("= ");
                if (rpos != std::string::npos)
                    ApplyColor(lineOffset + (int)rpos + 1, lineOffset + lineLen, RGB(180,180,180));
					ApplyHidden(lineOffset + (int)rpos + 1, lineOffset + lineLen, g_hideMarkup);
            }
        }
        // -- Bullet list --
        else if (line.size() >= 3 &&
                 (line.substr(0,3) == "  *" || line.substr(0,2) == "* "))
        {
            // Indent the paragraph via PARAFORMAT
            CHARRANGE cr;
            cr.cpMin = lineOffset;
            cr.cpMax = lineOffset + lineLen;
            SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);

            PARAFORMAT2 pf;
            ZeroMemory(&pf, sizeof(pf));
            pf.cbSize    = sizeof(pf);
            pf.dwMask    = PFM_STARTINDENT | PFM_OFFSET | PFM_NUMBERING;
            pf.wNumbering    = PFN_BULLET;
            pf.dxStartIndent = 360; // indent in twips (~0.25 inch)
            pf.dxOffset      = 360;
            SendMessage(g_hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);

            // Also tint the bullet marker grey
            int markerEnd = (line[0] == ' ') ? 3 : 2;
            ApplyColor(lineOffset, lineOffset + markerEnd, RGB(150,150,150));
			// If hidden then hide
			ApplyHidden(lineOffset, lineOffset + markerEnd, g_hideMarkup);
        }

        // -- Inline spans (bold/italic/underline) on all lines --
        ParseLineSpans(line, lineOffset);

		// Links [[url|label]] or [[url]]
		ParseLineLinks(line, lineOffset);

		// Citations
		ParseLineCitation(line, lineOffset, lineLen);

        // Advance past this line + the '\r' separator
        pos = eol + 1;
        // In some builds GetWindowText returns \r\n; skip extra \n
        if (pos < text.size() && text[pos] == '\n') pos++;

        // lineOffset advances by lineLen + 1 (for the '\r')
        lineOffset += lineLen + 1;
    }

    // --- 6. Restore selection and unfreeze ---
    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&savedSel);
    SendMessage(g_hEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hEdit, NULL, TRUE);
}

// =============================================================================
// FILE I/O
// Plain text read/write. The file is the markup, nothing else.
// =============================================================================

// --------------------------------------------------------------------------
// LoadFile: Read a file and put its contents into the RichEdit control.
// RichEdit wants '\r\n' line endings; we convert '\n' -> '\r\n'.
// --------------------------------------------------------------------------
static bool LoadFile(const std::string &path)
{
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD fileSize = GetFileSize(hFile, NULL);
    std::string raw(fileSize, '\0');
    DWORD bytesRead = 0;
    ReadFile(hFile, &raw[0], fileSize, &bytesRead, NULL);
    CloseHandle(hFile);
    raw.resize(bytesRead);

    // Normalise line endings: convert \r\n -> \n, then \r -> \n, then \n -> \r\n
    std::string normalised;
    normalised.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\r')
        {
            if (i + 1 < raw.size() && raw[i+1] == '\n') i++; // skip \r in \r\n
            normalised += "\r\n";
        }
        else if (raw[i] == '\n')
        {
            normalised += "\r\n";
        }
        else
        {
            normalised += raw[i];
        }
    }

    g_reformatting = true;
    SetWindowTextA(g_hEdit, normalised.c_str());
    g_reformatting = false;

    // Move caret to start
    CHARRANGE cr = {0, 0};
    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);

    ReapplyFormatting();
    return true;
}

// --------------------------------------------------------------------------
// SaveFile: Write the RichEdit plain text content to a file.
// We save with '\r\n' line endings (standard Windows).
// --------------------------------------------------------------------------
static bool SaveFile(const std::string &path)
{
    int textLen = GetWindowTextLength(g_hEdit);
    std::string text(textLen + 1, '\0');
    GetWindowTextA(g_hEdit, &text[0], textLen + 1);
    text.resize(textLen);

    // RichEdit uses '\r' internally; normalise to '\r\n' for the file
    std::string out;
    out.reserve(text.size() + 256);
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            out += "\r\n";
            if (i + 1 < text.size() && text[i+1] == '\n') i++;
        }
        else
        {
            out += text[i];
        }
    }

    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    WriteFile(hFile, out.c_str(), (DWORD)out.size(), &written, NULL);
    CloseHandle(hFile);
    return true;
}

// =============================================================================
// DIALOGS
// Common file open/save dialogs via commdlg.h
// =============================================================================

static bool ShowOpenDialog(char *outPath, DWORD bufSize)
{
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    outPath[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hMainWnd;
    ofn.lpstrFilter = "DokuWiki Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile   = outPath;
    ofn.nMaxFile    = bufSize;
    ofn.lpstrTitle  = "Open DokuWiki File";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "txt";
    return GetOpenFileNameA(&ofn) != 0;
}

static bool ShowSaveDialog(char *outPath, DWORD bufSize)
{
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    outPath[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hMainWnd;
    ofn.lpstrFilter = "DokuWiki Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile   = outPath;
    ofn.nMaxFile    = bufSize;
    ofn.lpstrTitle  = "Save DokuWiki File";
    ofn.Flags       = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "txt";
    return GetSaveFileNameA(&ofn) != 0;
}

// =============================================================================
// TITLE BAR
// Shows "DokuWriter - filename [*]" where [*] appears when modified.
// =============================================================================

static void UpdateTitleBar()
{
    std::string title = "DokuWriter - ";
    if (g_currentFile.empty())
        title += "(Untitled)";
    else
    {
        // Show just the filename, not the full path
        size_t slash = g_currentFile.find_last_of("\\/");
        if (slash != std::string::npos)
            title += g_currentFile.substr(slash + 1);
        else
            title += g_currentFile;
    }
    if (g_modified) title += " *";
    SetWindowTextA(g_hMainWnd, title.c_str());
}

// =============================================================================
// MARKUP INSERTION HELPERS
// When the user clicks Bold/Italic/etc., we wrap the current selection
// (or insert empty tokens at the caret) with the appropriate markup.
// =============================================================================

static void WrapSelectionWith(const char *openToken, const char *closeToken)
{
    CHARRANGE cr;
    SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);

    if (cr.cpMin == cr.cpMax)
    {
        // No selection: insert open+close and park caret between them
        // We do this via EM_REPLACESEL
        std::string insert = openToken;
        insert += closeToken;
        SendMessage(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)insert.c_str());

        // Move caret back by length of closeToken
        int newPos = cr.cpMin + (int)strlen(openToken);
        CHARRANGE newCr = {newPos, newPos};
        SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&newCr);
    }
    else
    {
        // Get selected text
        int selLen = cr.cpMax - cr.cpMin;
        std::string selText(selLen + 1, '\0');

        // EM_GETSELTEXT copies selection into buffer
        SendMessage(g_hEdit, EM_GETSELTEXT, 0, (LPARAM)&selText[0]);
        selText.resize(selLen);

        std::string replacement = openToken;
        replacement += selText;
        replacement += closeToken;

        SendMessage(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)replacement.c_str());

        // Re-select the original content (between the new tokens)
        CHARRANGE newCr;
        newCr.cpMin = cr.cpMin + (int)strlen(openToken);
        newCr.cpMax = newCr.cpMin + selLen;
        SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&newCr);
    }
    SetFocus(g_hEdit);
}

// --------------------------------------------------------------------------
// InsertLinePrefix: For headings and bullets, we operate on the whole
// current line. We find the start of the caret's line and insert a prefix,
// or if the prefix is already there, remove it (toggle).
// --------------------------------------------------------------------------
static void InsertHeading(const char *prefix, const char *suffix)
{
    // Get caret line index
    CHARRANGE cr;
    SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);
    int lineIdx = (int)SendMessage(g_hEdit, EM_EXLINEFROMCHAR, 0, cr.cpMin);
    int lineStart = (int)SendMessage(g_hEdit, EM_LINEINDEX, lineIdx, 0);
    int lineLen   = (int)SendMessage(g_hEdit, EM_LINELENGTH, lineStart, 0);

    // Fetch current line text
    std::string lineBuf(lineLen + 2, '\0');
    // EM_GETLINE requires first WORD of buffer to hold max chars
    *((WORD*)&lineBuf[0]) = (WORD)(lineLen + 1);
    int copied = (int)SendMessage(g_hEdit, EM_GETLINE, lineIdx, (LPARAM)&lineBuf[0]);
    lineBuf.resize(copied);

    // Select the whole line and replace it
    CHARRANGE lineCr = {lineStart, lineStart + lineLen};
    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&lineCr);

    std::string replacement = prefix;
    replacement += lineBuf;
    replacement += suffix;

    SendMessage(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)replacement.c_str());
    SetFocus(g_hEdit);
}

// =============================================================================
// COMMAND HANDLER
// Dispatches menu/toolbar commands.
// =============================================================================

static bool CheckSaveModified()
{
    if (!g_modified) return true; // Nothing to save, proceed

    int result = MessageBoxA(g_hMainWnd,
        "The document has unsaved changes. Save now?",
        "DokuWriter",
        MB_YESNOCANCEL | MB_ICONQUESTION);

    if (result == IDCANCEL) return false;
    if (result == IDNO)     return true;

    // IDYES: save
    if (g_currentFile.empty())
    {
        char path[MAX_PATH];
        if (!ShowSaveDialog(path, MAX_PATH)) return false;
        g_currentFile = path;
    }
    SaveFile(g_currentFile);
    g_modified = false;
    UpdateTitleBar();
    return true;
}

// =============================================================================
// Word Wrapping
// =============================================================================
static void SetWordWrap(bool wrap)
{
	if (wrap)
	{
		// NULL DC + 0 line width = wrap to window
		SendMessage(g_hEdit, EM_SETTARGETDEVICE, (WPARAM)NULL, 0);
		// Hide both scroll bars and only show the vertical one
		ShowScrollBar(g_hEdit, SB_BOTH, FALSE);
		ShowScrollBar(g_hEdit, SB_VERT, TRUE);
	}
	else
	{
		// NULL DC + 1 line width = no wrapping
		SendMessage(g_hEdit, EM_SETTARGETDEVICE, (WPARAM)NULL, 1);
		// Show all the scroll bars since word wrapping is off
		ShowScrollBar(g_hEdit, SB_BOTH, FALSE);
		ShowScrollBar(g_hEdit, SB_HORZ, TRUE);
		ShowScrollBar(g_hEdit, SB_VERT, TRUE);
	}

	// Force RichEdit to recalculate its line layout be resizing it.
	// We do this by briefly toggling its size, which flushes the layout engine.
	RECT rc;
	GetClientRect(g_hEdit, &rc);
	SetWindowPos(g_hEdit, NULL, 0, 0,
		rc.right - rc.left, rc.bottom - rc.top,
		SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
}


static void OnCommand(WPARAM wParam)
{
    WORD cmd = LOWORD(wParam);
    switch (cmd)
    {
    // -- File Menu --
    case IDM_FILE_NEW:
        if (!CheckSaveModified()) break;
        g_reformatting = true;
        SetWindowTextA(g_hEdit, "");
        g_reformatting = false;
        g_currentFile = "";
        g_modified    = false;
        UpdateTitleBar();
        break;

    case IDM_FILE_OPEN:
    {
        if (!CheckSaveModified()) break;
        char path[MAX_PATH];
        if (!ShowOpenDialog(path, MAX_PATH)) break;
        if (LoadFile(path))
        {
            g_currentFile = path;
            g_modified    = false;
            UpdateTitleBar();
        }
        else
        {
            MessageBoxA(g_hMainWnd, "Could not open file.", "DokuWriter", MB_ICONERROR);
        }
        break;
    }

    case IDM_FILE_SAVE:
    {
        if (g_currentFile.empty())
        {
            char path[MAX_PATH];
            if (!ShowSaveDialog(path, MAX_PATH)) break;
            g_currentFile = path;
        }
        if (SaveFile(g_currentFile))
        {
            g_modified = false;
            UpdateTitleBar();
        }
        else
        {
            MessageBoxA(g_hMainWnd, "Could not save file.", "DokuWriter", MB_ICONERROR);
        }
        break;
    }

    case IDM_FILE_SAVEAS:
    {
        char path[MAX_PATH];
        if (!ShowSaveDialog(path, MAX_PATH)) break;
        g_currentFile = path;
        if (SaveFile(g_currentFile))
        {
            g_modified = false;
            UpdateTitleBar();
        }
        else
        {
            MessageBoxA(g_hMainWnd, "Could not save file.", "DokuWriter", MB_ICONERROR);
        }
        break;
    }

    case IDM_FILE_EXIT:
        SendMessage(g_hMainWnd, WM_CLOSE, 0, 0);
        break;

    // -- Format Menu / Toolbar --
    case IDM_FORMAT_BOLD:
        WrapSelectionWith("**", "");
        break;

    case IDM_FORMAT_ITALIC:
        WrapSelectionWith("//", "");
        break;

    case IDM_FORMAT_UNDER:
        WrapSelectionWith("__", "");
        break;

    case IDM_FORMAT_H1:
        InsertHeading("====== ", "");
        break;

    case IDM_FORMAT_H2:
        InsertHeading("===== ", "");
        break;

    case IDM_FORMAT_H3:
        InsertHeading("==== ", "");
        break;

    case IDM_FORMAT_BULLET:
        InsertHeading("  * ", "");
        break;

    // -- Help Menu --
    case IDM_HELP_ABOUT:
        MessageBoxA(g_hMainWnd,
            "DokuWriter v0.2 by vmunix\r\n\r\n"
            "A simple WYSIWYG word processor\r\n"
            "using DokuWiki-style markup.\r\n\r\n"
            "Markup syntax:\r\n"
			"\n"
            "  **bold**\r\n"
            "  //italic//\r\n"
            "  __underline__\r\n"
            "  ====== Heading 1 ======\r\n"
            "  ===== Heading 2 =====\r\n"
            "  ==== Heading 3 ====\r\n"
            "  * Bullet item\r\n"
            "  (blank line = paragraph break)\r\n"
			"  [[URL|Label]]\r\n",
            "About DokuWriter",
            MB_ICONINFORMATION);
        break;

	case IDM_VIEW_HIDEMARKUP:
		{
			g_hideMarkup = !g_hideMarkup;

			// Update the checkmark on the menu item
			HMENU hMenu = GetMenu(g_hMainWnd);
			HMENU hView = GetSubMenu(hMenu, 1); // "View" is the second menu item (index 1)
			CheckMenuItem(hView, IDM_VIEW_HIDEMARKUP,
				MF_BYCOMMAND | (g_hideMarkup ? MF_CHECKED : MF_UNCHECKED));

			// Reformat to apply or remove CFE_HIDDEN on all tokens
			g_reformatting = true;
			ReapplyFormatting();
			g_reformatting = false;
			break;
		}
	case IDM_VIEW_WORDWRAP:
		{
			g_wordWrap = !g_wordWrap;

			HMENU hMenu = GetMenu(g_hMainWnd);
			HMENU hView = GetSubMenu(hMenu, 1); // View is index 1
			CheckMenuItem(hView, IDM_VIEW_WORDWRAP,
				MF_BYCOMMAND | (g_wordWrap ? MF_CHECKED : MF_UNCHECKED));

			SetWordWrap(g_wordWrap);
			break;
		}
	case IDM_VIEW_ZOOMIN:
		{
			if (g_baseFontSize < 72) // cap at 72pt
			{
				g_baseFontSize += 1;
				g_reformatting = true;
				ReapplyFormatting();
				g_reformatting = false;
			}
			break;
		}
		
	case IDM_VIEW_ZOOMOUT:
		{
			if (g_baseFontSize > 6) // floor at 6pt
			{
				g_baseFontSize -= 1;
				g_reformatting = true;
				ReapplyFormatting();
				g_reformatting = false;
			}
			break;
		}
    }
}

// =============================================================================
// MENU CREATION
// We build the menu in code (no .rc file needed).
// =============================================================================

static HMENU CreateAppMenu()
{
    HMENU hMenuBar = CreateMenu();

    // File menu
    HMENU hFile = CreatePopupMenu();
    AppendMenuA(hFile, MF_STRING, IDM_FILE_NEW,    "&New\tCtrl+N");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN,   "&Open...\tCtrl+O");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVE,   "&Save\tCtrl+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVEAS, "Save &As...");
    AppendMenuA(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXIT,   "E&xit");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFile, "&File");

	// View Menu
	HMENU hView = CreatePopupMenu();
	AppendMenuA(hView, MF_STRING, IDM_VIEW_HIDEMARKUP, "Hide &Markup\tCtrl+M");
	AppendMenuA(hView, MF_STRING, IDM_VIEW_WORDWRAP, "Word &Wrap\tCtrl+W");
	AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOMIN, "Zoom &In\tCtrl++");
	AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOMOUT, "Zoom &Out\t Ctrl+-");
	AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hView, "&View");

    // Format menu
    HMENU hFmt = CreatePopupMenu();
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_BOLD,   "&Bold\tCtrl+B");
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_ITALIC, "&Italic\tCtrl+I");
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_UNDER,  "&Underline\tCtrl+U");
    AppendMenuA(hFmt, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_H1,     "Heading &1\tCtrl+1");
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_H2,     "Heading &2\tCtrl+2");
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_H3,     "Heading &3\tCtrl+3");
    AppendMenuA(hFmt, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFmt, MF_STRING, IDM_FORMAT_BULLET, "B&ullet List\tCtrl+L");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFmt, "F&ormat");

    // Help menu
    HMENU hHelp = CreatePopupMenu();
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_ABOUT, "&About...");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, "&Help");

    return hMenuBar;
}

// =============================================================================
// TOOLBAR
// A simple hand-drawn toolbar using CreateWindow("BUTTON",...) buttons.
// We use text labels since we have no resource file for bitmaps.
// =============================================================================

#define TB_HEIGHT 28
#define TB_BTN_W  52
#define TB_BTN_H  24
#define TB_PAD    2

struct ToolbarButton {
    int   id;
    const char *label;
    const char *tip;
};

static const ToolbarButton g_tbButtons[] = {
    { IDM_FILE_NEW,    "New",    "New document"  },
    { IDM_FILE_OPEN,   "Open",   "Open file"     },
    { IDM_FILE_SAVE,   "Save",   "Save file"     },
    { 0,               NULL,     NULL            }, // separator
    { IDM_FORMAT_BOLD, "Bold",   "Bold (**)"     },
    { IDM_FORMAT_ITALIC,"Italic","Italic (//)"   },
    { IDM_FORMAT_UNDER,"Under",  "Underline (__)" },
    { 0,               NULL,     NULL            }, // separator
    { IDM_FORMAT_H1,   "H1",     "Heading 1"     },
    { IDM_FORMAT_H2,   "H2",     "Heading 2"     },
    { IDM_FORMAT_H3,   "H3",     "Heading 3"     },
    { IDM_FORMAT_BULLET,"List",  "Bullet list"   },
};
static const int g_tbCount = sizeof(g_tbButtons) / sizeof(g_tbButtons[0]);

static void CreateToolbar(HWND hParent)
{
    int x = TB_PAD;
    for (int i = 0; i < g_tbCount; i++)
    {
        if (g_tbButtons[i].id == 0)
        {
            // Separator: just a gap
            x += 8;
            continue;
        }
        CreateWindowA("BUTTON", g_tbButtons[i].label,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, TB_PAD, TB_BTN_W, TB_BTN_H,
            hParent, (HMENU)(UINT_PTR)g_tbButtons[i].id,
            g_hInst, NULL);
        x += TB_BTN_W + TB_PAD;
    }
}

// =============================================================================
// WINDOW PROCEDURE
// =============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // -------------------------------------------------------------------------
    case WM_CREATE:
    {
        g_hMainWnd = hwnd;

        // Load RichEdit library. Try 4.1 first (XP+), fall back to 2.0.
        if (!LoadLibraryA("Msftedit.dll"))
            LoadLibraryA("Riched20.dll");

        // Create toolbar
        CreateToolbar(hwnd);

        // Create RichEdit control (fills window below toolbar)
        g_hEdit = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            // MSFTEDIT_CLASS,      // "RICHEDIT50W" from Msftedit.dll
			"RICHEDIT50W",
            "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
            0, TB_HEIGHT, 100, 100,
            hwnd, (HMENU)IDC_RICHEDIT, g_hInst, NULL);

        // Enable rich-text mode and events
        SendMessage(g_hEdit, EM_SETTEXTMODE, TM_RICHTEXT, 0);
        SendMessage(g_hEdit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);

        // Set a comfortable margin
        SendMessage(g_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                    MAKELONG(12, 12));

        // Set default paragraph format (line spacing)
        PARAFORMAT2 pf;
        ZeroMemory(&pf, sizeof(pf));
        pf.cbSize    = sizeof(pf);
        pf.dwMask    = PFM_SPACEAFTER;
        pf.dySpaceAfter = TWIPS(4); // 4pt space after each paragraph
        SendMessage(g_hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);

        // Set undo limit
        SendMessage(g_hEdit, EM_SETUNDOLIMIT, 100, 0);

        SetFocus(g_hEdit);
        UpdateTitleBar();
        return 0;
    }

    // -------------------------------------------------------------------------
    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        MoveWindow(g_hEdit, 0, TB_HEIGHT, w, h - TB_HEIGHT, TRUE);
        return 0;
    }

    // -------------------------------------------------------------------------
    case WM_SETFOCUS:
        SetFocus(g_hEdit);
        return 0;

    // -------------------------------------------------------------------------
    case WM_COMMAND:
    {
        // Check if notification is from the RichEdit control
        if (LOWORD(wParam) == IDC_RICHEDIT)
        {
            if (HIWORD(wParam) == EN_CHANGE && !g_reformatting)
            {
                // Mark document as modified
                if (!g_modified)
                {
                    g_modified = true;
                    UpdateTitleBar();
                }
                // Restart the reformat timer on each keystroke
                SetTimer(hwnd, IDT_REFORMAT, REFORMAT_DELAY_MS, NULL);
            }
            break;
        }
        OnCommand(wParam);
        return 0;
    }

    // -------------------------------------------------------------------------
    // Keyboard shortcuts
    case WM_KEYDOWN:
        // These are handled by WM_COMMAND via accelerators; see TranslateAccelerator
        break;

    // -------------------------------------------------------------------------
    case WM_TIMER:
        if (wParam == IDT_REFORMAT)
        {
            KillTimer(hwnd, IDT_REFORMAT);
            g_reformatting = true;
            ReapplyFormatting();
            g_reformatting = false;
        }
        return 0;

    // -------------------------------------------------------------------------
    case WM_CLOSE:
        if (!CheckSaveModified()) return 0;
        DestroyWindow(hwnd);
        return 0;

    // -------------------------------------------------------------------------
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    // -------------------------------------------------------------------------
    // Draw a simple grey bar behind the toolbar buttons
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.bottom = TB_HEIGHT;
        HBRUSH hbr = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
        return 1;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// =============================================================================
// ACCELERATOR TABLE (keyboard shortcuts — built in code, no .rc needed)
// =============================================================================

static HACCEL CreateAcceleratorTable_()
{
    ACCEL accel[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW    },
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN   },
        { FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE   },
        { FVIRTKEY | FCONTROL, 'B', IDM_FORMAT_BOLD  },
        { FVIRTKEY | FCONTROL, 'I', IDM_FORMAT_ITALIC},
        { FVIRTKEY | FCONTROL, 'U', IDM_FORMAT_UNDER },
        { FVIRTKEY | FCONTROL, '1', IDM_FORMAT_H1    },
        { FVIRTKEY | FCONTROL, '2', IDM_FORMAT_H2    },
        { FVIRTKEY | FCONTROL, '3', IDM_FORMAT_H3    },
        { FVIRTKEY | FCONTROL, 'L', IDM_FORMAT_BULLET},
		{ FVIRTKEY | FCONTROL, 'M', IDM_VIEW_HIDEMARKUP },
		{ FVIRTKEY | FCONTROL, 'W', IDM_VIEW_WORDWRAP },
		{ FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_VIEW_ZOOMIN },
		{ FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_VIEW_ZOOMOUT },
    };
    return CreateAcceleratorTable(accel, sizeof(accel)/sizeof(accel[0]));
}

// =============================================================================
// WinMain — Entry Point
// =============================================================================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE /*hPrev*/,
                   LPSTR lpCmdLine, int nCmdShow)
{
    g_hInst = hInst;

    // Register window class
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "DokuWriterMain";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassA(&wc))
    {
        MessageBoxA(NULL, "RegisterClass failed.", "DokuWriter", MB_ICONERROR);
        return 1;
    }

    // Create main window
    g_hMainWnd = CreateWindowA(
        "DokuWriterMain",
        "DokuWriter",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 650,
        NULL, CreateAppMenu(), hInst, NULL);

    if (!g_hMainWnd)
    {
        MessageBoxA(NULL, "CreateWindow failed.", "DokuWriter", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    // If a file was passed on the command line, open it
    if (lpCmdLine && lpCmdLine[0] != '\0')
    {
        // Strip quotes if present
        std::string arg = lpCmdLine;
        if (!arg.empty() && arg[0] == '"')
        {
            arg = arg.substr(1);
            size_t q = arg.find('"');
            if (q != std::string::npos) arg = arg.substr(0, q);
        }
        if (LoadFile(arg))
        {
            g_currentFile = arg;
            g_modified    = false;
            UpdateTitleBar();
        }
    }

    // Accelerator table for keyboard shortcuts
    HACCEL hAccel = CreateAcceleratorTable_();

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (!TranslateAccelerator(g_hMainWnd, hAccel, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    DestroyAcceleratorTable(hAccel);
    return (int)msg.wParam;
}

// =============================================================================
// END OF FILE
// =============================================================================
