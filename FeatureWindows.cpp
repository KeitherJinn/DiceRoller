#include "FeatureWindows.h"
#include "DiceAnalysis.h"
#include "ResultStore.h"
#include "WindowLayout.h"
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <iomanip>
#include <numeric>
#include <sstream>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace ui {
namespace {
enum ControlId { FilterName=200, FilterDice, DateFrom, DateTo, Unsaved, Filter, Batches,
    RollNumber, Previous, Next, Detail, Summary, Chart, Reuse, Export, Close,
    FormulaA, FormulaB, ModeB, Calculate, ProbabilityText, CancelCalculation };
const wchar_t* CompareText(dice::Compare value) {
    switch (value) {
    case dice::Compare::AtLeast: return L">="; case dice::Compare::AtMost: return L"<=";
    case dice::Compare::Equal: return L"="; case dice::Compare::Greater: return L">";
    case dice::Compare::Less: return L"<";
    }
    return L"?";
}
std::wstring GetText(HWND control) {
    std::wstring value(GetWindowTextLengthW(control)+1, L'\0');
    value.resize(GetWindowTextW(control,value.data(),static_cast<int>(value.size())));
    return value;
}
std::wstring Number(long double value, int precision=3) {
    std::wostringstream text; text << std::fixed << std::setprecision(precision) << value; return text.str();
}
void GroupDetail(std::wostringstream& text, const dice::Spec& spec, const dice::Group& group) {
    std::size_t index=0;
    for (const auto& term : dice::DiceTerms(spec)) {
        text << term.count << L"d" << term.sides;
        if (term.keep) text << (term.keepHighest ? L"kh" : L"kl") << term.keep;
        text << L": ";
        for (int i=0;i<term.count;++i,++index) {
            if (i) text << L", ";
            const bool kept=group.kept.empty() || group.kept.at(index);
            if (!kept) text << L"[";
            text << group.dice.at(index);
            if (!kept) text << L" dropped]";
            else if (spec.successThreshold) text << (group.dice.at(index)>=spec.successThreshold ? L" success" : L" fail");
        }
        text << L"\r\n";
    }
    text << (spec.successThreshold ? L"Successes: " : L"Subtotal: ") << group.total << L"\r\n";
}
struct Panel {
    HWND window=nullptr,owner=nullptr; HFONT font=nullptr; UINT dpi=96;
    bool probability=false,layingOut=false,refreshing=false,closing=false;
    int scrollX=0,scrollY=0,contentWidth=0,contentHeight=0;
    const std::vector<dice::HistoryEntry>* entries=nullptr;
    std::vector<std::size_t> visible;
    HistorySelection selection;
    std::size_t roll=0;
    dice::Request request;
    std::map<std::int64_t,double> distributionA,distributionB;
    std::atomic<bool> cancel{false};
    std::future<std::pair<dice::Probability,dice::Probability>> future;
    ~Panel() { cancel=true; if(future.valid()) future.wait(); if(font) DeleteObject(font); }
};
HWND Child(const Panel& panel,int id) { return GetDlgItem(panel.window,id); }
int Scale(const Panel& panel,int n) { return MulDiv(n,static_cast<int>(panel.dpi),96); }
void SetText(const Panel& panel,int id,const std::wstring& text) { SetWindowTextW(Child(panel,id),text.c_str()); }
void Font(Panel& panel) {
    auto font=CreateFontW(-MulDiv(10,static_cast<int>(panel.dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!font) throw std::runtime_error("Could not create a window font.");
    for(auto child=GetWindow(panel.window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    if(panel.font) DeleteObject(panel.font); panel.font=font;
}
void LayoutPanel(Panel& panel) {
    if(panel.layingOut) return;
    panel.layingOut=true;
    RECT client{}; GetClientRect(panel.window,&client);
    for(int i=0;i<3;++i) {
        panel.contentWidth=(std::max)(static_cast<int>(client.right),Scale(panel,760));
        panel.contentHeight=(std::max)(static_cast<int>(client.bottom),Scale(panel,panel.probability?690:850));
        panel.scrollX=std::clamp(panel.scrollX,0,(std::max)(0,panel.contentWidth-static_cast<int>(client.right)));
        panel.scrollY=std::clamp(panel.scrollY,0,(std::max)(0,panel.contentHeight-static_cast<int>(client.bottom)));
        SCROLLINFO x{sizeof(x),SIF_RANGE|SIF_PAGE|SIF_POS,0,panel.contentWidth-1,static_cast<UINT>(client.right),panel.scrollX,0};
        SCROLLINFO y{sizeof(y),SIF_RANGE|SIF_PAGE|SIF_POS,0,panel.contentHeight-1,static_cast<UINT>(client.bottom),panel.scrollY,0};
        SetScrollInfo(panel.window,SB_HORZ,&x,TRUE); SetScrollInfo(panel.window,SB_VERT,&y,TRUE);
        GetClientRect(panel.window,&client);
    }
    const int width=MulDiv(panel.contentWidth,96,static_cast<int>(panel.dpi));
    const int height=MulDiv(panel.contentHeight,96,static_cast<int>(panel.dpi));
    auto move=[&](int id,int x,int y,int w,int h) {
        MoveWindow(Child(panel,id),Scale(panel,x)-panel.scrollX,Scale(panel,y)-panel.scrollY,Scale(panel,w),Scale(panel,h),FALSE);
    };
    if(panel.probability) {
        move(500,16,16,width-32,44);
        move(501,16,72,120,24); move(FormulaA,140,68,width-156,26);
        move(502,16,108,120,24); move(FormulaB,140,104,width-352,26); move(ModeB,width-200,104,184,180);
        move(Calculate,16,144,144,28); move(CancelCalculation,172,144,112,28);
        move(ProbabilityText,16,184,width-32,210);
        move(Chart,16,406,width-32,height-468);
    } else {
        move(500,16,16,66,22); move(FilterName,86,12,220,26);
        move(501,322,16,70,22); move(FilterDice,396,12,width-412,26);
        move(502,16,52,66,22); move(DateFrom,86,48,132,26);
        move(503,230,52,28,22); move(DateTo,264,48,132,26);
        move(Unsaved,412,48,166,26); move(Filter,width-160,48,144,26);
        move(504,16,82,width-32,24);
        move(Batches,16,110,width-32,154);
        move(Reuse,16,274,176,28); move(Export,204,274,176,28);
        move(Summary,16,314,width-32,92); move(Chart,16,418,width-32,140);
        move(505,16,570,80,24); move(RollNumber,96,566,96,26);
        move(Previous,204,566,90,28); move(Next,306,566,90,28);
        move(506,412,570,width-428,24);
        move(Detail,16,606,width-32,height-668);
    }
    move(Close,width-136,height-46,120,28);
    panel.layingOut=false;
    RedrawWindow(panel.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
}
void EnsurePanelFocus(Panel& panel) {
    auto focus=GetFocus(); if(!focus || !IsChild(panel.window,focus)) return;
    RECT r{},c{}; GetWindowRect(focus,&r); MapWindowPoints(nullptr,panel.window,reinterpret_cast<POINT*>(&r),2);
    GetClientRect(panel.window,&c); const int x=panel.scrollX,y=panel.scrollY;
    if(r.top<0) panel.scrollY+=r.top; else if(r.bottom>c.bottom) panel.scrollY+=(std::min)(r.top,r.bottom-c.bottom);
    if(r.left<0) panel.scrollX+=r.left; else if(r.right>c.right) panel.scrollX+=(std::min)(r.left,r.right-c.right);
    if(x!=panel.scrollX || y!=panel.scrollY) LayoutPanel(panel);
}
void DrawChart(Panel& panel,HDC dc,RECT bounds) {
    FillRect(dc,&bounds,GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
    const auto oldFont=SelectObject(dc,panel.font);
    if(panel.distributionA.empty()) {
        DrawTextW(dc,L"Select a batch or calculate a probability distribution.",-1,&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,oldFont); return;
    }
    auto minimum=panel.distributionA.begin()->first,maximum=panel.distributionA.rbegin()->first;
    if(!panel.distributionB.empty()) { minimum=(std::min)(minimum,panel.distributionB.begin()->first); maximum=(std::max)(maximum,panel.distributionB.rbegin()->first); }
    // Integer subtraction first also preserves narrow ranges near INT64_MAX.
    const long double span=static_cast<long double>(static_cast<std::uint64_t>(maximum)-static_cast<std::uint64_t>(minimum))+1;
    const int bins=static_cast<int>((std::min)(20.0L,span));
    std::vector<long double> a(bins),b(bins);
    auto collect=[&](const auto& source,auto& destination) {
        for(const auto& [value,weight]:source) {
            const auto distance=static_cast<long double>(static_cast<std::uint64_t>(value)-static_cast<std::uint64_t>(minimum));
            const auto bin=(std::min)(bins-1,static_cast<int>(distance*bins/span));
            destination[bin]+=weight;
        }
    };
    collect(panel.distributionA,a); collect(panel.distributionB,b);
    long double peak=0; for(int i=0;i<bins;++i) peak=(std::max)({peak,a[i],b[i]});
    RECT title=bounds; title.left+=8; title.top+=4;
    auto label=L"Distribution: A (blue) / B (orange) | peak bin "+Number(peak*100,1)+L"%";
    DrawTextW(dc,label.c_str(),-1,&title,DT_LEFT|DT_TOP|DT_SINGLELINE);
    const int left=bounds.left+8,right=bounds.right-8,top=bounds.top+Scale(panel,30),bottom=bounds.bottom-Scale(panel,30);
    const auto blue=CreateSolidBrush(RGB(30,105,190)),orange=CreateSolidBrush(RGB(210,116,24));
    for(int i=0;i<bins;++i) {
        const int x=left+(right-left)*i/bins,end=left+(right-left)*(i+1)/bins;
        const int mid=panel.distributionB.empty()?end:x+(end-x)/2;
        RECT ar{x+1,bottom-static_cast<int>((bottom-top)*a[i]/(peak?peak:1)),mid-1,bottom}; FillRect(dc,&ar,blue);
        if(!panel.distributionB.empty()) { RECT br{mid+1,bottom-static_cast<int>((bottom-top)*b[i]/(peak?peak:1)),end-1,bottom}; FillRect(dc,&br,orange); }
    }
    DeleteObject(blue); DeleteObject(orange);
    RECT axis{left,bottom+2,right,bounds.bottom};
    const auto range=std::to_wstring(minimum)+L" ... "+std::to_wstring(maximum)+L"  ("+std::to_wstring(bins)+L" equal-width bins; frequencies normalized per batch)";
    DrawTextW(dc,range.c_str(),-1,&axis,DT_CENTER|DT_SINGLELINE);
    SelectObject(dc,oldFont);
}
std::vector<std::size_t> Selected(Panel& panel) {
    std::vector<std::size_t> result;
    int row=-1; while((row=ListView_GetNextItem(Child(panel,Batches),row,LVNI_SELECTED))!=-1)
        result.push_back(panel.visible.at(row));
    return result;
}
void RefreshDetails(Panel& panel,bool resetRoll) {
    const auto selection=Selected(panel);
    if(resetRoll) panel.roll=0;
    panel.distributionA.clear(); panel.distributionB.clear();
    std::wstring summary;
    for(std::size_t i=0;i<(std::min)(selection.size(),std::size_t(2));++i) {
        const auto& entry=panel.entries->at(selection[i]);
        const auto stats=dice::Summarize(entry.batch);
        summary+=(i?L"B: ":L"A: ")+BatchSummary(entry)+L"\r\n";
        auto& distribution=i?panel.distributionB:panel.distributionA;
        if(stats.count) for(const auto& [value,count]:stats.frequencies) distribution[value]=static_cast<double>(count)/stats.count;
    }
    if(selection.size()>2) summary+=L"Statistics compare the first two selected batches; Export includes all selected batches.";
    SetText(panel,Summary,summary);
    EnableWindow(Child(panel,Reuse),selection.size()==1);
    const bool exportable=!selection.empty() && std::all_of(selection.begin(),selection.end(),[&](std::size_t i) {return !panel.entries->at(i).batch.results.empty();});
    EnableWindow(Child(panel,Export),exportable);
    if(!selection.empty()) {
        const auto& entry=panel.entries->at(selection.front());
        panel.roll=entry.batch.results.empty()?0:(std::min)(panel.roll,entry.batch.results.size()-1);
        SetText(panel,RollNumber,entry.batch.results.empty()?L"":std::to_wstring(panel.roll+1));
        SetText(panel,506,L"/ "+std::to_wstring(entry.batch.results.size())+L" rolls; [ ] = dropped dice");
        SetText(panel,Detail,entry.batch.results.empty()?L"No completed rolls.":FullRollDetail(entry,panel.roll));
        EnableWindow(Child(panel,Previous),panel.roll>0);
        EnableWindow(Child(panel,Next),panel.roll+1<entry.batch.results.size());
    } else {
        SetText(panel,RollNumber,L""); SetText(panel,506,L"No rolls selected");
        SetText(panel,Detail,L"No matching batches. Clear filters to see all history.");
        EnableWindow(Child(panel,Previous),FALSE); EnableWindow(Child(panel,Next),FALSE);
    }
    InvalidateRect(Child(panel,Chart),nullptr,TRUE);
}
void FilterHistory(Panel& panel) {
    dice::HistoryQuery query;
    query.name=GetText(Child(panel,FilterName)); query.notation=GetText(Child(panel,FilterDice));
    query.dateFrom=dice::Utf8(dice::Trim(GetText(Child(panel,DateFrom))));
    query.dateTo=dice::Utf8(dice::Trim(GetText(Child(panel,DateTo))));
    query.unsavedOnly=SendMessageW(Child(panel,Unsaved),BM_GETCHECK,0,0)==BST_CHECKED;
    auto visible=dice::QueryHistory(*panel.entries,query);
    panel.refreshing=true;
    ListView_DeleteAllItems(Child(panel,Batches)); panel.visible=std::move(visible);
    for(std::size_t row=0;row<panel.visible.size();++row) {
        const auto& entry=panel.entries->at(panel.visible[row]);
        auto date=dice::Wide(entry.created);
        LVITEMW item{}; item.mask=LVIF_TEXT; item.iItem=static_cast<int>(row); item.pszText=date.data();
        ListView_InsertItem(Child(panel,Batches),&item);
        std::wstring cells[]={entry.batch.request.name,entry.batch.request.notation,
            std::to_wstring(entry.batch.results.size())+L"/"+std::to_wstring(entry.batch.request.times),entry.saved?L"Exported":L"Pending export"};
        for(int col=0;col<4;++col) ListView_SetItemText(Child(panel,Batches),static_cast<int>(row),col+1,cells[col].data());
    }
    panel.refreshing=false;
    if(!panel.visible.empty()) ListView_SetItemState(Child(panel,Batches),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    RefreshDetails(panel,true);
}
void CalculateProbability(Panel& panel) {
    if(panel.future.valid()) return;
    auto a=panel.request,b=panel.request;
    a.notation=dice::Trim(GetText(Child(panel,FormulaA))); a.spec=dice::Parse(a.notation);
    b.notation=dice::Trim(GetText(Child(panel,FormulaB))); b.spec=dice::Parse(b.notation);
    a.spec.successThreshold=b.spec.successThreshold=panel.request.spec.successThreshold;
    b.mode=static_cast<dice::Mode>(SendMessageW(Child(panel,ModeB),CB_GETCURSEL,0,0));
    a.times=b.times=1; dice::Validate(a); dice::Validate(b);
    panel.cancel=false;
    panel.distributionA.clear(); panel.distributionB.clear(); InvalidateRect(Child(panel,Chart),nullptr,TRUE);
    panel.future=std::async(std::launch::async,[a,b,&panel] {
        const auto cancelled=[&panel] { return panel.cancel.load(); };
        auto first=dice::Analyze(a,cancelled); auto second=dice::Analyze(b,cancelled);
        return std::make_pair(std::move(first),std::move(second));
    });
    for(int id:{FormulaA,FormulaB,ModeB,Calculate}) EnableWindow(Child(panel,id),FALSE);
    EnableWindow(Child(panel,CancelCalculation),TRUE);
    SetText(panel,ProbabilityText,L"Calculating in the background. Complex formulas use a clearly labelled bounded simulation.");
}
std::wstring ProbabilityDescription(const dice::Probability& value,const wchar_t* label) {
    std::wstring text=label;
    text+=value.exact?L" — EXACT":L" — ESTIMATE";
    text+=L"; range "+std::to_wstring(value.minimum)+L".."+std::to_wstring(value.maximum)+L"; mean "+Number(value.mean)+L"\r\n";
    if(value.hasTarget) {
        text+=L"Target probability "+Number(value.matchedProportion*100,3)+L"%";
        if(!value.exact) text+=L"; 95% bound +/- "+Number(value.marginOfError95*100,3)+L" percentage points";
        text+=L"\r\n";
    }
    if(!value.exact) text+=L"Samples: "+std::to_wstring(value.samples)+L". Range is theoretical; mean and bars are sample estimates.\r\n";
    return text+L"\r\n";
}
void Tick(Panel& panel) {
    if(!panel.future.valid() || panel.future.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
    try {
        auto values=panel.future.get();
        panel.distributionA=values.first.distribution; panel.distributionB=values.second.distribution;
        SetText(panel,ProbabilityText,ProbabilityDescription(values.first,L"A")+ProbabilityDescription(values.second,L"B")+
            L"The same target and pool threshold apply to A and B. These are model probabilities, separate from recorded batch statistics.");
        InvalidateRect(Child(panel,Chart),nullptr,TRUE);
    } catch(const dice::Cancelled&) { SetText(panel,ProbabilityText,L"Calculation cancelled."); }
    catch(const std::exception& e) { SetText(panel,ProbabilityText,dice::Wide(e.what())); }
    for(int id:{FormulaA,FormulaB,ModeB,Calculate}) EnableWindow(Child(panel,id),TRUE);
    EnableWindow(Child(panel,CancelCalculation),FALSE);
    if(panel.closing) DestroyWindow(panel.window);
}
void SetupPanel(Panel& panel) {
    auto add=[&](int id,const wchar_t* type,const wchar_t* text,DWORD style,DWORD ex=0) {
        auto control=CreateWindowExW(ex,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,panel.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!control) throw std::runtime_error("Could not create a feature window control.");
        return control;
    };
    auto edit=[&](int id,const wchar_t* text,int limit=1024) {
        auto child=add(id,L"EDIT",text,WS_TABSTOP|ES_AUTOHSCROLL,WS_EX_CLIENTEDGE);
        SendMessageW(child,EM_LIMITTEXT,limit,0); return child;
    };
    auto label=[&](int id,const wchar_t* text) { add(id,L"STATIC",text,SS_NOPREFIX); };
    auto button=[&](int id,const wchar_t* text) { add(id,L"BUTTON",text,WS_TABSTOP|BS_PUSHBUTTON); };
    if(panel.probability) {
        auto rules=L"A mode: "+dice::Wide(dice::ModeName(panel.request.mode))+L". ";
        rules+=panel.request.target.enabled?L"Target: final value "+std::wstring(CompareText(panel.request.target.comparison))+L" "+std::to_wstring(panel.request.target.value)+L". ":L"Target disabled. ";
        rules+=panel.request.spec.successThreshold?L"Pool success threshold: "+std::to_wstring(panel.request.spec.successThreshold):L"Ordinary totals (pool off).";
        label(500,rules.c_str()); label(501,L"Formula A:"); edit(FormulaA,panel.request.notation.c_str());
        label(502,L"Formula B:"); edit(FormulaB,panel.request.notation.c_str());
        auto modes=add(ModeB,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP);
        for(const auto* mode:{L"Normal",L"Advantage",L"Disadvantage"}) SendMessageW(modes,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(mode));
        SendMessageW(modes,CB_SETCURSEL,static_cast<int>(panel.request.mode),0);
        button(Calculate,L"Calculate / compare"); button(CancelCalculation,L"Cancel calculation");
        EnableWindow(Child(panel,CancelCalculation),FALSE);
        add(ProbabilityText,L"EDIT",L"Change either formula or mode B, then Calculate. To change the target or pool threshold, close this window and adjust the main controls.",
            WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,WS_EX_CLIENTEDGE);
    } else {
        label(500,L"Name:"); edit(FilterName,L"",128); label(501,L"Formula:"); edit(FilterDice,L"");
        label(502,L"UTC date:"); edit(DateFrom,L"",10); label(503,L"to"); edit(DateTo,L"",10);
        add(Unsaved,L"BUTTON",L"Pending export only",BS_AUTOCHECKBOX|WS_TABSTOP);
        button(Filter,L"Apply filters"); label(504,L"Dates: YYYY-MM-DD, blank = any. Ctrl/Shift select batches; the first two selections are compared below.");
        auto list=add(Batches,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SHOWSELALWAYS,WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[]={L"Created (UTC)",L"Name",L"Formula",L"Completed",L"CSV status"};
        const int widths[]={168,140,170,92,130};
        for(int i=0;i<5;++i) { LVCOLUMNW col{}; col.mask=LVCF_TEXT|LVCF_WIDTH; col.pszText=const_cast<wchar_t*>(titles[i]); col.cx=Scale(panel,widths[i]); ListView_InsertColumn(list,i,&col); }
        button(Reuse,L"Use parameters for new roll"); button(Export,L"Export selected batches");
        add(Summary,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,WS_EX_CLIENTEDGE);
        label(505,L"Roll #:"); edit(RollNumber,L"1",5); button(Previous,L"Previous"); button(Next,L"Next");
        label(506,L"Full details, including dropped dice");
        auto detail=add(Detail,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,WS_EX_CLIENTEDGE);
        SendMessageW(detail,EM_LIMITTEXT,1000000,0);
    }
    add(Chart,L"STATIC",L"",SS_OWNERDRAW);
    button(Close,L"Close");
    Font(panel); LayoutPanel(panel);
    if(!SetTimer(panel.window,1,50,nullptr)) throw std::runtime_error("Could not start the feature-window timer.");
    if(!panel.probability) FilterHistory(panel);
}
LRESULT CALLBACK PanelProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto panel=reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    try {
        if(message==WM_NCCREATE) { panel=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); panel->window=window;panel->dpi=GetDpiForWindow(window);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(panel)); }
        if(!panel) return DefWindowProcW(window,message,wp,lp);
        switch(message) {
        case WM_CREATE: SetupPanel(*panel); return 0;
        case WM_SIZE: LayoutPanel(*panel); return 0;
        case WM_DPICHANGED: {
            const UINT oldDpi=panel->dpi;
            panel->dpi=HIWORD(wp); Font(*panel);
            if(!panel->probability) for(int i=0;i<5;++i)
                ListView_SetColumnWidth(Child(*panel,Batches),i,MulDiv(ListView_GetColumnWidth(Child(*panel,Batches),i),static_cast<int>(panel->dpi),static_cast<int>(oldDpi)));
            auto r=*reinterpret_cast<RECT*>(lp); r=FitToWorkArea(r,WorkAreaFor(r));
            SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
            LayoutPanel(*panel); return 0;
        }
        case WM_VSCROLL: case WM_HSCROLL: {
            const int bar=message==WM_VSCROLL?SB_VERT:SB_HORZ;
            SCROLLINFO s{sizeof(s),SIF_ALL};GetScrollInfo(window,bar,&s);int pos=s.nPos;
            switch(LOWORD(wp)) {
            case SB_LINEUP:pos-=Scale(*panel,32);break;case SB_LINEDOWN:pos+=Scale(*panel,32);break;
            case SB_PAGEUP:pos-=s.nPage;break;case SB_PAGEDOWN:pos+=s.nPage;break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=s.nTrackPos;break;
            case SB_TOP:pos=0;break;case SB_BOTTOM:pos=s.nMax;break;
            }
            (bar==SB_VERT?panel->scrollY:panel->scrollX)=pos;LayoutPanel(*panel);return 0;
        }
        case WM_MOUSEWHEEL: panel->scrollY-=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*Scale(*panel,64);LayoutPanel(*panel);return 0;
        case WM_DRAWITEM: { auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp); if(draw->CtlID==Chart) {DrawChart(*panel,draw->hDC,draw->rcItem);return TRUE;} break; }
        case WM_NOTIFY:
            if(reinterpret_cast<NMHDR*>(lp)->idFrom==Batches && reinterpret_cast<NMHDR*>(lp)->code==LVN_ITEMCHANGED && !panel->refreshing) RefreshDetails(*panel,true);
            return 0;
        case WM_TIMER: Tick(*panel);return 0;
        case WM_COMMAND: {
            const int id=LOWORD(wp);
            if(id==RollNumber && HIWORD(wp)==EN_KILLFOCUS) {
                try { const auto n=std::stoul(GetText(Child(*panel,RollNumber)));panel->roll=n?n-1:0; }catch(...) {panel->roll=0;}
                RefreshDetails(*panel,false);return 0;
            }
            if(lp && HIWORD(wp)!=BN_CLICKED) return 0;
            switch(id) {
            case Filter:FilterHistory(*panel);break;
            case Previous: if(panel->roll) --panel->roll;RefreshDetails(*panel,false);break;
            case Next:++panel->roll;RefreshDetails(*panel,false);break;
            case Reuse:case Export:
                panel->selection.indices=Selected(*panel);
                if(panel->selection.indices.empty() || (id==Reuse && panel->selection.indices.size()!=1)) break;
                panel->selection.action=id==Reuse?HistorySelection::Action::Reuse:HistorySelection::Action::Export;
                DestroyWindow(window);break;
            case Calculate:CalculateProbability(*panel);break;
            case CancelCalculation:panel->cancel=true;break;
            case IDOK:if(panel->probability) CalculateProbability(*panel);else FilterHistory(*panel);break;
            case IDCANCEL:case Close:SendMessageW(window,WM_CLOSE,0,0);break;
            }
            return 0;
        }
        case WM_CLOSE:
            if(panel->future.valid()) { panel->closing=true;panel->cancel=true;SetText(*panel,ProbabilityText,L"Stopping calculation..."); }
            else DestroyWindow(window);return 0;
        case WM_DESTROY:KillTimer(window,1);return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
        }
    } catch(const std::exception& e) { MessageBoxW(window,dice::Wide(e.what()).c_str(),L"Dice Roller",MB_OK|MB_ICONERROR);if(message==WM_CREATE)return -1; }
    return DefWindowProcW(window,message,wp,lp);
}
void RunPanel(Panel& panel) {
    INITCOMMONCONTROLSEX init{sizeof(init),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&init);
    WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=PanelProc;
    wc.lpszClassName=L"DiceRollerFeatures";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
    if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Could not register the feature window.");
    RECT owner{};GetWindowRect(panel.owner,&owner);
    const int dpi=static_cast<int>(GetDpiForWindow(panel.owner));
    RECT rect{owner.left+20,owner.top+20,owner.left+20+MulDiv(900,dpi,96),owner.top+20+MulDiv(panel.probability?780:960,dpi,96)};
    rect=FitToWorkArea(rect,WorkAreaFor(rect));
    auto window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,panel.probability?L"Probability and comparison":L"History, recovery and statistics",
        WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_HSCROLL,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,panel.owner,nullptr,wc.hInstance,&panel);
    if(!window) throw std::runtime_error("Could not open the feature window.");
    EnableWindow(panel.owner,FALSE);ShowWindow(window,SW_SHOW);SetFocus(Child(panel,panel.probability?FormulaA:FilterName));
    MSG msg{};int status=0;
    while(IsWindow(window) && (status=GetMessageW(&msg,nullptr,0,0))>0) {
        auto previousFocus=GetFocus();
        if(!IsDialogMessageW(window,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(IsWindow(window) && previousFocus!=GetFocus()) EnsurePanelFocus(panel);
    }
    if(IsWindow(window)) DestroyWindow(window);
    EnableWindow(panel.owner,TRUE);SetActiveWindow(panel.owner);
    if(status==0) PostQuitMessage(static_cast<int>(msg.wParam));
    if(status==-1) throw std::runtime_error("Feature window message loop failed.");
}
}
std::wstring FullRollDetail(const dice::HistoryEntry& entry,std::size_t index) {
    const auto& request=entry.batch.request; const auto& result=entry.batch.results.at(index);
    std::wostringstream text;
    text << request.name << L" | " << request.notation << L" | " << dice::Wide(dice::ModeName(request.mode))
        << L" | roll " << index+1 << L"\r\nBatch " << dice::Wide(entry.id) << L" | " << dice::Wide(result.timestamp) << L" (local time)\r\n";
    text << L"Group 1:\r\n";GroupDetail(text,request.spec,result.first);
    if(result.second) {text << L"Group 2:\r\n";GroupDetail(text,request.spec,*result.second);}
    text << L"Chosen: " << result.chosen << L"; modifier: " << request.spec.modifier << L"; final: " << result.finalValue << L"\r\n";
    if(request.target.enabled) text << L"Target " << CompareText(request.target.comparison) << L" " << request.target.value << L": "
        << (dice::EvaluateTarget(result.finalValue,request.target)?L"MET":L"NOT MET") << L"; margin " << dice::TargetMargin(result.finalValue,request.target) << L"\r\n";
    text << (entry.saved?L"Exported to: ":L"Pending CSV export. ") << entry.exportedPath;
    return text.str();
}
std::wstring BatchSummary(const dice::HistoryEntry& entry) {
    const auto stats=dice::Summarize(entry.batch);
    if(!stats.count) return entry.batch.request.name+L": no completed rolls.";
    auto text=entry.batch.request.name+L" ["+entry.batch.request.notation+L", "+dice::Wide(dice::ModeName(entry.batch.request.mode))+L"] n="+std::to_wstring(stats.count)+
        L"; min="+std::to_wstring(stats.minimum)+L"; max="+std::to_wstring(stats.maximum)+L"; mean="+Number(stats.mean)+
        L"; median="+Number(stats.median)+L"; SD="+Number(stats.standardDeviation);
    if(entry.batch.request.spec.successThreshold) text+=L"; pool >= "+std::to_wstring(entry.batch.request.spec.successThreshold);
    if(stats.hasTarget) text+=L"; target "+std::wstring(CompareText(entry.batch.request.target.comparison))+L" "+std::to_wstring(entry.batch.request.target.value)+L": "+std::to_wstring(stats.matched)+L"/"+std::to_wstring(stats.count)+L" ("+Number(stats.matchedProportion*100,2)+L"%)";
    return text;
}
HistorySelection ShowHistory(HWND owner,const std::vector<dice::HistoryEntry>& entries) {
    Panel panel;panel.owner=owner;panel.entries=&entries;RunPanel(panel);return panel.selection;
}
void ShowProbability(HWND owner,const dice::Request& request) {
    Panel panel;panel.owner=owner;panel.probability=true;panel.request=request;RunPanel(panel);
}
std::optional<dice::ImportStrategy> ChooseImportStrategy(HWND owner,const dice::ImportPreview& preview) {
    auto text=L"Incoming: "+std::to_wstring(preview.incomingCount)+L"; new names: "+std::to_wstring(preview.newCount)+
        L"; conflicts: "+std::to_wstring(preview.conflictCount)+L".\n";
    for(const auto& name:preview.conflicts) text+=L"\n• "+name;
    text+=L"\n\nKeep/replace total: "+std::to_wstring(preview.resultCount)+L"; rename total: "+std::to_wstring(preview.renamedResultCount)+L". Limit: 50 presets.";
    TASKDIALOG_BUTTON buttons[]={{100,L"Keep existing presets"},{101,L"Rename incoming duplicates"},{102,L"Replace existing duplicates"}};
    TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.hwndParent=owner;dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle=L"Import preset preview";dialog.pszMainInstruction=L"Review duplicates and choose an import strategy";dialog.pszContent=text.c_str();
    dialog.cButtons=3;dialog.pButtons=buttons;dialog.nDefaultButton=100;dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;
    int chosen=0;if(FAILED(TaskDialogIndirect(&dialog,&chosen,nullptr,nullptr))) throw std::runtime_error("Could not show the import preview.");
    if(chosen==100) return dice::ImportStrategy::KeepExisting;
    if(chosen==101) return dice::ImportStrategy::RenameIncoming;
    if(chosen==102) return dice::ImportStrategy::ReplaceExisting;
    return std::nullopt;
}
}
