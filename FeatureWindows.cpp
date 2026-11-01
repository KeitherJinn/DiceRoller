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
#include <set>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace ui {
namespace {
enum ControlId { FilterName=200, FilterDice, DateFrom, DateTo, Unsaved, Filter, Batches,
    RollNumber, Previous, Next, Detail, Summary, Chart, Reuse, Export, Close,
    FormulaA, FormulaB, ModeB, Calculate, ProbabilityText, CancelCalculation,
    Reload, IncludeArchived, Archive, PagePrevious, PageNext, PageLabel, Frequencies,
    CopyFrequencies, HistoryStatus, Body, ModeLabel };
constexpr std::size_t PageSize = 100;
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
struct CachedBatch {
    dice::HistoryEntry entry;
    dice::Statistics statistics;
    std::wstring summary;
};
struct HistoryWork {
    enum class Kind { Details, Reload, Archive } kind=Kind::Details;
    std::vector<std::shared_ptr<CachedBatch>> details;
    dice::HistoryIndex index;
    std::wstring message;
    std::size_t statisticsBuilt=0;
    bool replaceIndex=false;
};
struct Panel {
    HWND window=nullptr,owner=nullptr; HFONT font=nullptr; UINT dpi=96;
    bool probability=false,layingOut=false,refreshing=false,closing=false,stale=false,detailsPending=false;
    HWND body=nullptr;
    int footerHeight=0,wheelRemainder=0;
    int scrollX=0,scrollY=0,contentWidth=0,contentHeight=0;
    const std::vector<dice::HistoryEntry>* entries=nullptr;
    const std::vector<dice::HistoryEntry>* dirty=nullptr;
    std::function<void()> exportPreflight;
    std::vector<dice::HistorySummary> pendingExportMarks;
    std::wstring historyNotice;
    std::filesystem::path root;
    std::vector<dice::HistorySummary> index;
    std::vector<std::size_t> matches;
    std::size_t page=0,statisticsBuilds=0;
    std::vector<std::shared_ptr<CachedBatch>> cache,shown;
    std::vector<std::size_t> visible;
    HistorySelection selection;
    std::size_t roll=0;
    dice::Request request,calculatedA,calculatedB;
    std::map<std::int64_t,double> distributionA,distributionB;
    std::atomic<bool> cancel{false};
    std::future<std::pair<dice::Probability,dice::Probability>> future;
    std::future<HistoryWork> historyFuture;
    ~Panel() { cancel=true; if(future.valid()) future.wait(); if(historyFuture.valid()) historyFuture.wait(); if(font) DeleteObject(font); }
};
HWND Child(const Panel& panel,int id) { auto child=GetDlgItem(panel.window,id);return child?child:GetDlgItem(panel.body,id); }
bool Working(const Panel& panel) {return panel.future.valid() || panel.historyFuture.valid();}
void LayoutPanel(Panel& panel);
void EnsurePanelFocus(Panel& panel);
void RefreshDetails(Panel& panel,bool resetRoll);
void FilterHistory(Panel& panel,bool resetPage=true,const std::vector<std::string>& keep={});
void Tick(Panel& panel);
int Scale(const Panel& panel,int n) { return MulDiv(n,static_cast<int>(panel.dpi),96); }
void SetText(const Panel& panel,int id,const std::wstring& text) { SetWindowTextW(Child(panel,id),text.c_str()); }
void Font(Panel& panel) {
    auto font=CreateFontW(-MulDiv(10,static_cast<int>(panel.dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!font) throw std::runtime_error("Could not create a window font.");
    for(auto parent:{panel.window,panel.body})
        for(auto child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    if(panel.font) DeleteObject(panel.font); panel.font=font;
}
int TextHeight(const Panel& panel,int id,int width) {
    auto control=Child(panel,id);auto dc=GetDC(control);const auto old=SelectObject(dc,panel.font);
    RECT r{0,0,(std::max)(1,width),0};const auto text=GetText(control);
    DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,old);ReleaseDC(control,dc);
    return (std::max)(Scale(panel,20),static_cast<int>(r.bottom))+Scale(panel,4);
}
int ButtonWidth(const Panel& panel,int id) {
    auto control=Child(panel,id);auto dc=GetDC(control);const auto old=SelectObject(dc,panel.font);
    RECT r{};const auto text=GetText(control);DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_SINGLELINE);
    SelectObject(dc,old);ReleaseDC(control,dc);
    return (std::max)(Scale(panel,78),static_cast<int>(r.right)+Scale(panel,24));
}
int LineHeight(const Panel& panel) {
    auto dc=GetDC(panel.window);const auto old=SelectObject(dc,panel.font);TEXTMETRICW metric{};GetTextMetricsW(dc,&metric);
    SelectObject(dc,old);ReleaseDC(panel.window,dc);
    return (std::max)(Scale(panel,28),static_cast<int>(metric.tmHeight)+Scale(panel,10));
}
void LayoutPanel(Panel& panel) {
    if(panel.layingOut || !panel.body || !panel.font)return;
    panel.layingOut=true;RECT client{};GetClientRect(panel.window,&client);
    const int gap=Scale(panel,8),margin=Scale(panel,12),line=LineHeight(panel);
    const std::vector<int> actions=panel.probability?std::vector<int>{Calculate,CancelCalculation,Close}:
        std::vector<int>{Reuse,Export,Archive,CancelCalculation,Close};
    std::vector<RECT> footer;int x=margin,y=gap;
    for(int id:actions) {
        const int width=(std::min)(ButtonWidth(panel,id),(std::max)(1,static_cast<int>(client.right)-2*margin));
        if(x>margin && x+width>client.right-margin){x=margin;y+=line+gap;}
        footer.push_back({x,y,x+width,y+line});x+=width+gap;
    }
    panel.footerHeight=y+line+gap;const int bodyHeight=(std::max)(1,static_cast<int>(client.bottom)-panel.footerHeight);
    MoveWindow(panel.body,0,0,client.right,bodyHeight,FALSE);
    for(std::size_t i=0;i<actions.size();++i){const auto& r=footer[i];MoveWindow(Child(panel,actions[i]),r.left,bodyHeight+r.top,r.right-r.left,r.bottom-r.top,FALSE);}
    for(int pass=0;pass<3;++pass) {
        RECT viewport{};GetClientRect(panel.body,&viewport);
        const int width=(std::max)(static_cast<int>(viewport.right),Scale(panel,320)),inner=width-2*margin;
        const bool wide=width>=Scale(panel,620);panel.contentWidth=width;y=margin;
        auto place=[&](int id,int left,int top,int w,int h){MoveWindow(Child(panel,id),left-panel.scrollX,top-panel.scrollY,(std::max)(1,w),(std::max)(1,h),FALSE);};
        auto label=[&](int id){const int h=TextHeight(panel,id,inner);place(id,margin,y,inner,h);y+=h+gap;};
        auto field=[&](int labelId,int editId){const int w=Scale(panel,86);place(labelId,margin,y+Scale(panel,3),w,line);place(editId,margin+w,y,inner-w,line);y+=line+gap;};
        auto buttons=[&](std::initializer_list<int> ids){
            int left=margin;
            for(int id:ids){const int w=(std::min)(ButtonWidth(panel,id),inner);if(left>margin && left+w>width-margin){left=margin;y+=line+gap;}place(id,left,y,w,line);left+=w+gap;}
            y+=line+gap;
        };
        if(panel.probability) {
            label(500);field(501,FormulaA);field(502,FormulaB);
            place(ModeLabel,margin,y+Scale(panel,3),Scale(panel,86),line);
            place(ModeB,margin+Scale(panel,86),y,inner-Scale(panel,86),Scale(panel,180));y+=line+gap;
            place(ProbabilityText,margin,y,inner,Scale(panel,180));y+=Scale(panel,180)+gap;
        } else {
            if(wide) {
                const int half=(inner-gap)/2,w=Scale(panel,70);
                place(500,margin,y,w,line);place(FilterName,margin+w,y,half-w,line);
                place(501,margin+half+gap,y,w,line);place(FilterDice,margin+half+gap+w,y,half-w,line);y+=line+gap;
                place(502,margin,y,w,line);place(DateFrom,margin+w,y,half-w,line);
                place(503,margin+half+gap,y,w,line);place(DateTo,margin+half+gap+w,y,half-w,line);y+=line+gap;
            } else {field(500,FilterName);field(501,FilterDice);field(502,DateFrom);field(503,DateTo);}
            place(Unsaved,margin,y,inner,line);y+=line+gap;
            place(IncludeArchived,margin,y,inner,line);y+=line+gap;
            buttons({Filter,Reload});label(504);label(HistoryStatus);
            place(Batches,margin,y,inner,Scale(panel,180));y+=Scale(panel,180)+gap;
            buttons({PagePrevious,PageNext});label(PageLabel);
            place(Summary,margin,y,inner,Scale(panel,100));y+=Scale(panel,100)+gap;
        }
        place(Chart,margin,y,inner,Scale(panel,150));y+=Scale(panel,150)+gap;
        label(507);buttons({CopyFrequencies});
        place(Frequencies,margin,y,inner,Scale(panel,150));y+=Scale(panel,150)+gap;
        if(!panel.probability){field(505,RollNumber);buttons({Previous,Next});label(506);place(Detail,margin,y,inner,Scale(panel,210));y+=Scale(panel,210)+gap;}
        panel.contentHeight=y+margin;
        const int oldX=panel.scrollX,oldY=panel.scrollY;
        panel.scrollX=std::clamp(panel.scrollX,0,(std::max)(0,width-static_cast<int>(viewport.right)));
        panel.scrollY=std::clamp(panel.scrollY,0,(std::max)(0,panel.contentHeight-static_cast<int>(viewport.bottom)));
        SCROLLINFO sx{sizeof(sx),SIF_RANGE|SIF_PAGE|SIF_POS,0,width-1,static_cast<UINT>(viewport.right),panel.scrollX,0};
        SCROLLINFO sy{sizeof(sy),SIF_RANGE|SIF_PAGE|SIF_POS,0,panel.contentHeight-1,static_cast<UINT>(viewport.bottom),panel.scrollY,0};
        SetScrollInfo(panel.body,SB_HORZ,&sx,TRUE);SetScrollInfo(panel.body,SB_VERT,&sy,TRUE);
        RECT after{};GetClientRect(panel.body,&after);
        if(pass && oldX==panel.scrollX && oldY==panel.scrollY && after.right==viewport.right && after.bottom==viewport.bottom)break;
    }
    panel.layingOut=false;RedrawWindow(panel.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
}
void EnsurePanelFocus(Panel& panel) {
    auto focus=GetFocus();if(!focus || !IsChild(panel.body,focus))return;
    RECT r{},c{};GetWindowRect(focus,&r);MapWindowPoints(nullptr,panel.body,reinterpret_cast<POINT*>(&r),2);
    GetClientRect(panel.body,&c);const int x=panel.scrollX,y=panel.scrollY;
    if(r.top<0)panel.scrollY+=r.top;else if(r.bottom>c.bottom)panel.scrollY+=(std::min)(r.top,r.bottom-c.bottom);
    if(r.left<0)panel.scrollX+=r.left;else if(r.right>c.right)panel.scrollX+=(std::min)(r.left,r.right-c.right);
    if(x!=panel.scrollX || y!=panel.scrollY)LayoutPanel(panel);
}
void ScrollPanel(Panel& panel,int bar,int command) {
    SCROLLINFO s{sizeof(s),SIF_ALL};GetScrollInfo(panel.body,bar,&s);int pos=s.nPos;
    switch(command){
    case SB_LINEUP:pos-=Scale(panel,32);break;case SB_LINEDOWN:pos+=Scale(panel,32);break;
    case SB_PAGEUP:pos-=s.nPage;break;case SB_PAGEDOWN:pos+=s.nPage;break;
    case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=s.nTrackPos;break;
    case SB_TOP:pos=0;break;case SB_BOTTOM:pos=s.nMax;break;default:return;
    }
    (bar==SB_VERT?panel.scrollY:panel.scrollX)=pos;LayoutPanel(panel);
}
LRESULT CALLBACK BodyProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto panel=reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){panel=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(panel));}
    if(panel){
        if(message==WM_COMMAND || message==WM_NOTIFY || message==WM_DRAWITEM)return SendMessageW(panel->window,message,wp,lp);
        if(message==WM_VSCROLL || message==WM_HSCROLL){ScrollPanel(*panel,message==WM_VSCROLL?SB_VERT:SB_HORZ,LOWORD(wp));return 0;}
        if(message==WM_MOUSEWHEEL){
            panel->wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);
            while(panel->wheelRemainder>=WHEEL_DELTA){ScrollPanel(*panel,SB_VERT,SB_LINEUP);panel->wheelRemainder-=WHEEL_DELTA;}
            while(panel->wheelRemainder<=-WHEEL_DELTA){ScrollPanel(*panel,SB_VERT,SB_LINEDOWN);panel->wheelRemainder+=WHEEL_DELTA;}return 0;
        }
    }
    return DefWindowProcW(window,message,wp,lp);
}
void DrawChart(Panel& panel,HDC dc,RECT bounds) {
    FillRect(dc,&bounds,GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
    const auto oldFont=SelectObject(dc,panel.font);
    if(panel.distributionA.empty()) {
        DrawTextW(dc,L"Select a batch or calculate a probability distribution.",-1,&bounds,DT_CENTER|DT_WORDBREAK);
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
    auto label=L"A: blue / B: orange; peak "+Number(peak*100,1)+L"%";
    DrawTextW(dc,label.c_str(),-1,&title,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_END_ELLIPSIS);
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
    const auto range=std::to_wstring(minimum)+L" ... "+std::to_wstring(maximum)+L"  ("+std::to_wstring(bins)+L" bins; values below)";
    DrawTextW(dc,range.c_str(),-1,&axis,DT_CENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    SelectObject(dc,oldFont);
}
std::vector<std::size_t> Selected(Panel& panel) {
    std::vector<std::size_t> result;
    int row=-1; while((row=ListView_GetNextItem(Child(panel,Batches),row,LVNI_SELECTED))!=-1)
        result.push_back(panel.visible.at(row));
    return result;
}
std::vector<std::string> SelectedIds(Panel& panel) {
    std::vector<std::string> result;for(auto i:Selected(panel))result.push_back(panel.index[i].id);return result;
}
bool ExportMarkPending(const Panel& panel,const std::string& id) {
    return std::any_of(panel.pendingExportMarks.begin(),panel.pendingExportMarks.end(),[&](const auto& mark){return mark.id==id;});
}
void EnableHistory(Panel& panel) {
    const bool busy=Working(panel);const auto selected=Selected(panel);
    for(int id:{FilterName,FilterDice,DateFrom,DateTo,Unsaved,IncludeArchived,Filter,Reload,Batches,PagePrevious,PageNext,RollNumber,Previous,Next})EnableWindow(Child(panel,id),!busy);
    EnableWindow(Child(panel,Reload),!busy && !panel.root.empty());EnableWindow(Child(panel,IncludeArchived),!busy && !panel.root.empty());
    EnableWindow(Child(panel,PagePrevious),!busy && panel.page>0);
    EnableWindow(Child(panel,PageNext),!busy && (panel.page+1)*PageSize<panel.matches.size());
    EnableWindow(Child(panel,Reuse),!busy && selected.size()==1);
    const bool nonempty=!selected.empty() && std::all_of(selected.begin(),selected.end(),[&](auto i){return panel.index[i].resultCount>0;});
    EnableWindow(Child(panel,Export),!busy && nonempty);
    const bool archivable=!panel.root.empty() && !selected.empty() && std::all_of(selected.begin(),selected.end(),[&](auto i){const auto& e=panel.index[i];return e.saved && !e.archived && e.localPersisted && !ExportMarkPending(panel,e.id);});
    EnableWindow(Child(panel,Archive),!busy && archivable);EnableWindow(Child(panel,CancelCalculation),busy && !panel.closing);
    const bool hasRoll=!busy && !panel.shown.empty() && !panel.shown.front()->entry.batch.results.empty();
    EnableWindow(Child(panel,RollNumber),hasRoll);EnableWindow(Child(panel,Previous),hasRoll && panel.roll>0);
    EnableWindow(Child(panel,Next),hasRoll && panel.roll+1<panel.shown.front()->entry.batch.results.size());
}
void FrequencyText(Panel& panel) {
    std::set<std::int64_t> values;for(const auto& item:panel.distributionA)values.insert(item.first);for(const auto& item:panel.distributionB)values.insert(item.first);
    std::wostringstream text;text << (panel.probability?L"Value\tA probability %\tB probability %\r\n":L"Value\tA count\tA %\tB count\tB %\r\n");
    for(auto value:values){
        const auto a=panel.distributionA.find(value),b=panel.distributionB.find(value);
        const double pa=a==panel.distributionA.end()?0:a->second,pb=b==panel.distributionB.end()?0:b->second;text << value << L'\t';
        if(!panel.probability){
            auto count=[&](std::size_t index){if(index>=panel.shown.size())return std::size_t(0);const auto& map=panel.shown[index]->statistics.frequencies;const auto it=map.find(value);return it==map.end()?std::size_t(0):it->second;};
            text << count(0) << L'\t' << Number(pa*100,6) << L'\t' << count(1) << L'\t' << Number(pb*100,6);
        }else text << Number(pa*100,8) << L'\t' << Number(pb*100,8);
        text << L"\r\n";
    }
    SetText(panel,Frequencies,values.empty()?L"":text.str());EnableWindow(Child(panel,CopyFrequencies),!values.empty());
}
void RenderRoll(Panel& panel) {
    if(panel.shown.empty()){SetText(panel,RollNumber,L"");SetText(panel,506,L"No result selected");SetText(panel,Detail,L"Select a batch to load its full details.");}
    else{
        const auto& entry=panel.shown.front()->entry;const auto size=entry.batch.results.size();panel.roll=size?(std::min)(panel.roll,size-1):0;
        SetText(panel,RollNumber,size?std::to_wstring(panel.roll+1):L"");
        SetText(panel,506,L"/ "+std::to_wstring(size)+L" rolls. [ ] marks dropped dice.");
        SetText(panel,Detail,size?FullRollDetail(entry,panel.roll):L"No completed rolls.");
    }
    EnableHistory(panel);
}
void DisplayDetails(Panel& panel,std::vector<std::shared_ptr<CachedBatch>> details) {
    panel.shown=std::move(details);panel.cache=panel.shown;
    panel.distributionA.clear();panel.distributionB.clear();std::wstring summary;
    for(std::size_t i=0;i<panel.shown.size();++i){
        const auto& item=*panel.shown[i];summary+=(i?L"B: ":L"A: ")+item.summary+L"\r\n";
        auto& distribution=i?panel.distributionB:panel.distributionA;
        if(item.statistics.count)for(const auto& [value,count]:item.statistics.frequencies)distribution[value]=static_cast<double>(count)/item.statistics.count;
    }
    if(Selected(panel).size()>2)summary+=L"Only the first two selections are compared; Export uses every selected row on this page.";
    const auto selected=Selected(panel);
    const bool pending=std::any_of(selected.begin(),selected.end(),[&](auto i){return ExportMarkPending(panel,panel.index[i].id);});
    std::wstring status=pending?L"CSV export succeeded; its status still needs saving. Close the main window to retry, or export this batch again. Archive is unavailable until status is saved.":
        L"Full details loaded. Statistics are reused while paging through rolls.";
    if(!panel.historyNotice.empty())status+=L"\r\n"+panel.historyNotice;
    SetText(panel,HistoryStatus,status);
    SetText(panel,Summary,summary);FrequencyText(panel);RenderRoll(panel);InvalidateRect(Child(panel,Chart),nullptr,TRUE);
}
std::wstring DescribeBatch(const dice::HistoryEntry& entry,const dice::Statistics& stats) {
    if(!stats.count) return entry.batch.request.name+L": no completed rolls.";
    auto text=entry.batch.request.name+L" ["+entry.batch.request.notation+L", "+dice::Wide(dice::ModeName(entry.batch.request.mode))+L"] n="+std::to_wstring(stats.count)+
        L"; min="+std::to_wstring(stats.minimum)+L"; max="+std::to_wstring(stats.maximum)+L"; mean="+stats.meanText+
        L"; median="+stats.medianText+L"; SD="+Number(stats.standardDeviation);
    if(entry.batch.request.spec.successThreshold) text+=L"; pool >= "+std::to_wstring(entry.batch.request.spec.successThreshold);
    if(stats.hasTarget) text+=L"; target "+std::wstring(CompareText(entry.batch.request.target.comparison))+L" "+std::to_wstring(entry.batch.request.target.value)+L": "+std::to_wstring(stats.matched)+L"/"+std::to_wstring(stats.count)+L" ("+Number(stats.matchedProportion*100,2)+L"%)";
    return text;
}
void RefreshDetails(Panel& panel,bool resetRoll) {
    panel.detailsPending=false;if(resetRoll)panel.roll=0;if(Working(panel))return;
    const auto selected=Selected(panel);if(selected.empty()){DisplayDetails(panel,{});return;}
    std::vector<dice::HistorySummary> wanted;
    for(std::size_t i=0;i<(std::min)(selected.size(),std::size_t(2));++i)wanted.push_back(panel.index[selected[i]]);
    std::vector<std::shared_ptr<CachedBatch>> cached;
    for(const auto& summary:wanted){
        const auto it=std::find_if(panel.cache.begin(),panel.cache.end(),[&](const auto& c){return c->entry.id==summary.id && c->entry.archived==summary.archived && c->entry.batch.results.size()==summary.resultCount;});
        if(it!=panel.cache.end())cached.push_back(*it);
    }
    if(cached.size()==wanted.size()){DisplayDetails(panel,std::move(cached));return;}
    panel.cancel=false;const auto cache=panel.cache;const auto root=panel.root;const auto legacy=panel.entries;const auto dirty=panel.dirty;
    panel.shown.clear();panel.distributionA.clear();panel.distributionB.clear();FrequencyText(panel);
    SetText(panel,Summary,L"Loading the selected batches...");SetText(panel,Detail,L"Loading full details...");
    SetText(panel,RollNumber,L"");InvalidateRect(Child(panel,Chart),nullptr,TRUE);
    panel.historyFuture=std::async(std::launch::async,[wanted,cache,root,legacy,dirty,&panel]{
        HistoryWork work;
        for(const auto& summary:wanted){
            if(panel.cancel.load())throw dice::Cancelled();
            const auto found=std::find_if(cache.begin(),cache.end(),[&](const auto& c){return c->entry.id==summary.id && c->entry.archived==summary.archived && c->entry.batch.results.size()==summary.resultCount;});
            if(found!=cache.end()){work.details.push_back(*found);continue;}
            auto item=std::make_shared<CachedBatch>();bool fromMemory=false;
            for(auto source:{dirty,legacy})if(source){
                const auto it=std::find_if(source->begin(),source->end(),[&](const auto& entry){return entry.id==summary.id;});
                if(it!=source->end()){item->entry=*it;fromMemory=true;break;}
            }
            if(!fromMemory)item->entry=dice::ReadHistoryEntry(root,summary.id,summary.archived,[&panel]{return panel.cancel.load();});
            item->entry.saved=summary.saved;item->entry.exportedPath=summary.exportedPath;
            if(panel.cancel.load())throw dice::Cancelled();
            item->statistics=dice::Summarize(item->entry.batch);item->summary=DescribeBatch(item->entry,item->statistics);
            ++work.statisticsBuilt;work.details.push_back(std::move(item));
        }
        return work;
    });
    SetText(panel,HistoryStatus,L"Loading selected batches in the background...");EnableHistory(panel);SetFocus(Child(panel,CancelCalculation));LayoutPanel(panel);
}
dice::HistoryQuery CurrentQuery(const Panel& panel) {
    dice::HistoryQuery query;query.name=GetText(Child(panel,FilterName));query.notation=GetText(Child(panel,FilterDice));
    query.dateFrom=dice::Utf8(dice::Trim(GetText(Child(panel,DateFrom))));query.dateTo=dice::Utf8(dice::Trim(GetText(Child(panel,DateTo))));
    query.unsavedOnly=SendMessageW(Child(panel,Unsaved),BM_GETCHECK,0,0)==BST_CHECKED;return query;
}
void FilterHistory(Panel& panel,bool resetPage,const std::vector<std::string>& keep) {
    if(Working(panel))return;
    auto matches=dice::QueryHistoryIndex(panel.index,CurrentQuery(panel));
    const bool include=SendMessageW(Child(panel,IncludeArchived),BM_GETCHECK,0,0)==BST_CHECKED;
    matches.erase(std::remove_if(matches.begin(),matches.end(),[&](auto i){return panel.index[i].archived && !include;}),matches.end());
    if(resetPage)panel.page=0;panel.matches=std::move(matches);
    if(!resetPage && !keep.empty()) {
        const auto anchor=std::find_if(panel.matches.begin(),panel.matches.end(),[&](auto i){return panel.index[i].id==keep.front();});
        if(anchor!=panel.matches.end())panel.page=static_cast<std::size_t>(anchor-panel.matches.begin())/PageSize;
    }
    if(panel.page*PageSize>=panel.matches.size())panel.page=panel.matches.empty()?0:(panel.matches.size()-1)/PageSize;
    panel.visible.clear();
    for(auto i=panel.page*PageSize;i<(std::min)(panel.matches.size(),(panel.page+1)*PageSize);++i)panel.visible.push_back(panel.matches[i]);
    panel.refreshing=true;ListView_DeleteAllItems(Child(panel,Batches));bool selected=false;
    for(std::size_t row=0;row<panel.visible.size();++row){
        const auto& entry=panel.index[panel.visible[row]];auto date=dice::Wide(entry.created);
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(row);item.pszText=date.data();ListView_InsertItem(Child(panel,Batches),&item);
        const std::wstring status=entry.archived?L"Archived":!entry.localPersisted?L"Memory only":ExportMarkPending(panel,entry.id)?L"Exported; status pending":entry.saved?L"Exported":L"Pending export";
        std::wstring cells[]={entry.request.name,entry.request.notation,std::to_wstring(entry.resultCount)+L"/"+std::to_wstring(entry.request.times),status};
        for(int col=0;col<4;++col)ListView_SetItemText(Child(panel,Batches),static_cast<int>(row),col+1,cells[col].data());
        if(std::find(keep.begin(),keep.end(),entry.id)!=keep.end()){ListView_SetItemState(Child(panel,Batches),static_cast<int>(row),LVIS_SELECTED,LVIS_SELECTED);selected=true;}
    }
    if(!selected && !panel.visible.empty())ListView_SetItemState(Child(panel,Batches),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    panel.refreshing=false;
    SetText(panel,PageLabel,L"Page "+std::to_wstring(panel.page+1)+L" / "+std::to_wstring((std::max)(std::size_t(1),(panel.matches.size()+PageSize-1)/PageSize))+
        L"; "+std::to_wstring(panel.matches.size())+L" matching batches. Up to 100 rows per page.");
    RefreshDetails(panel,true);LayoutPanel(panel);
}
void MergeDirty(Panel& panel,std::vector<dice::HistorySummary>& index) {
    if(panel.dirty)for(const auto& entry:*panel.dirty){
        auto summary=dice::SummaryOf(entry);summary.localPersisted=false;
        const auto old=std::find_if(index.begin(),index.end(),[&](const auto& e){return e.id==entry.id;});
        if(old==index.end())index.push_back(std::move(summary));else *old=std::move(summary);
    }
    for(const auto& mark:panel.pendingExportMarks){
        const auto found=std::find_if(index.begin(),index.end(),[&](const auto& entry){return entry.id==mark.id;});
        if(found!=index.end()){found->saved=mark.saved;found->exportedPath=mark.exportedPath;}
    }
}
void ReloadHistory(Panel& panel,bool archive) {
    if(Working(panel) || panel.root.empty())return;
    std::vector<dice::HistorySummary> records;
    if(archive){
        for(auto i:Selected(panel)){
            const auto& record=panel.index[i];
            if(!record.saved || record.archived || !record.localPersisted || ExportMarkPending(panel,record.id))
                throw std::invalid_argument("Archive requires locally stored, exported batches with saved export status. Retry saving pending status first.");
            records.push_back(record);
        }
        if(records.empty())return;
    }
    dice::QueryHistoryIndex(panel.index,CurrentQuery(panel));
    panel.cancel=false;panel.historyNotice.clear();const auto root=panel.root;const auto original=panel.index;
    panel.historyFuture=std::async(std::launch::async,[root,records,original,archive,&panel]{
        HistoryWork work;work.kind=archive?HistoryWork::Kind::Archive:HistoryWork::Kind::Reload;std::size_t completed=0;
        if(archive){
            work.index.entries=original;
            for(const auto& record:records){
                if(panel.cancel.load())break;
                try{
                    const auto moved=dice::ArchiveHistoryEntry(root,record.id);
                    const auto found=std::find_if(work.index.entries.begin(),work.index.entries.end(),[&](const auto& entry){return entry.id==moved.id;});
                    if(found!=work.index.entries.end())*found=moved;
                    ++completed;work.replaceIndex=true;
                }
                catch(const std::exception& e){work.message=L"Archive stopped after "+std::to_wstring(completed)+L" batch(es): "+dice::Wide(e.what());break;}
            }
            if(work.message.empty())work.message=(panel.cancel.load()?L"Archive cancelled. ":L"")+std::wstring(L"Archived ")+std::to_wstring(completed)+L" batch(es).";
        }
        try{
            auto scanned=dice::ScanHistory(root,true,[&panel]{return panel.cancel.load();});
            // An unreadable file/folder is not evidence that its known record was
            // removed. Retain that metadata so a failed refresh keeps selection.
            if(!scanned.warnings.empty())for(const auto& known:archive?work.index.entries:original){
                const auto found=std::find_if(scanned.entries.begin(),scanned.entries.end(),[&](const auto& entry){return entry.id==known.id;});
                if(found==scanned.entries.end())scanned.entries.push_back(known);
            }
            work.index=std::move(scanned);work.replaceIndex=true;
        }
        catch(const dice::Cancelled&){if(!archive)throw;work.message+=L" Reload cancelled; use Reload to refresh.";}
        catch(const std::exception& e){if(!archive)throw;work.message+=L" Reload failed: "+dice::Wide(e.what());}
        return work;
    });
    SetText(panel,HistoryStatus,archive?L"Archiving selected exported batches...":L"Reloading history metadata...");
    EnableHistory(panel);SetFocus(Child(panel,CancelCalculation));LayoutPanel(panel);
}
bool CommitRollNumber(Panel& panel) {
    if(panel.shown.empty() || panel.shown.front()->entry.batch.results.empty())return false;
    const auto text=dice::Trim(GetText(Child(panel,RollNumber)));std::size_t value=0;bool valid=!text.empty();
    for(wchar_t ch:text){if(ch<L'0' || ch>L'9' || value>10000){valid=false;break;}value=value*10+static_cast<std::size_t>(ch-L'0');}
    const auto count=panel.shown.front()->entry.batch.results.size();
    if(!valid || value<1 || value>count){
        SetText(panel,HistoryStatus,L"Enter a complete roll number between 1 and "+std::to_wstring(count)+L".");
        SetWindowTextW(Child(panel,RollNumber),std::to_wstring(panel.roll+1).c_str());LayoutPanel(panel);return false;
    }
    panel.roll=value-1;RenderRoll(panel);return true;
}
void MarkStale(Panel& panel) {
    if(panel.refreshing || Working(panel))return;
    panel.stale=true;panel.distributionA.clear();panel.distributionB.clear();FrequencyText(panel);
    SetText(panel,ProbabilityText,L"Parameters changed. Calculate again to update the probabilities and table.");
    InvalidateRect(Child(panel,Chart),nullptr,TRUE);
}
void CalculateProbability(Panel& panel) {
    if(Working(panel)) return;
    auto a=panel.request,b=panel.request;
    a.notation=dice::Trim(GetText(Child(panel,FormulaA))); a.spec=dice::Parse(a.notation);
    b.notation=dice::Trim(GetText(Child(panel,FormulaB))); b.spec=dice::Parse(b.notation);
    a.spec.successThreshold=b.spec.successThreshold=panel.request.spec.successThreshold;
    b.mode=static_cast<dice::Mode>(SendMessageW(Child(panel,ModeB),CB_GETCURSEL,0,0));
    a.times=b.times=1; dice::Validate(a); dice::Validate(b);
    panel.calculatedA=a;panel.calculatedB=b;panel.cancel=false;panel.stale=false;
    panel.distributionA.clear(); panel.distributionB.clear(); InvalidateRect(Child(panel,Chart),nullptr,TRUE);
    FrequencyText(panel);
    panel.future=std::async(std::launch::async,[a,b,&panel] {
        const auto cancelled=[&panel] { return panel.cancel.load(); };
        auto first=dice::Analyze(a,cancelled); auto second=dice::Analyze(b,cancelled);
        return std::make_pair(std::move(first),std::move(second));
    });
    for(int id:{FormulaA,FormulaB,ModeB,Calculate}) EnableWindow(Child(panel,id),FALSE);
    EnableWindow(Child(panel,CancelCalculation),TRUE);
    SetFocus(Child(panel,CancelCalculation));
    SetText(panel,ProbabilityText,L"Calculating in the background. Complex formulas use a clearly labelled bounded simulation.");
}
std::wstring ProbabilityDescription(const dice::Probability& value,const wchar_t* label,const dice::Request& request) {
    std::wstring text=std::wstring(label)+L": "+request.notation+L" ["+dice::Wide(dice::ModeName(request.mode))+L"]";
    text+=value.exact?L" — EXACT":L" — ESTIMATE";
    text+=L"; range "+std::to_wstring(value.minimum)+L".."+std::to_wstring(value.maximum)+L"; mean "+value.meanText+L"\r\n";
    if(value.hasTarget) {
        text+=L"Target probability "+Number(value.matchedProportion*100,3)+L"%";
        if(!value.exact) text+=L"; 95% bound +/- "+Number(value.marginOfError95*100,3)+L" percentage points";
        text+=L"\r\n";
    }
    if(!value.exact) text+=L"Samples: "+std::to_wstring(value.samples)+L". Range is theoretical; mean and bars are sample estimates.\r\n";
    return text+L"\r\n";
}
void Tick(Panel& panel) {
    if(panel.detailsPending && !Working(panel) && !panel.closing){panel.detailsPending=false;RefreshDetails(panel,true);}
    if(panel.future.valid() && panel.future.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
    try {
        auto values=panel.future.get();
        panel.distributionA=values.first.distribution; panel.distributionB=values.second.distribution;
        SetText(panel,ProbabilityText,ProbabilityDescription(values.first,L"A",panel.calculatedA)+ProbabilityDescription(values.second,L"B",panel.calculatedB)+
            L"The same target and pool threshold apply to A and B. These are model probabilities, separate from recorded batch statistics.");
        InvalidateRect(Child(panel,Chart),nullptr,TRUE);
        FrequencyText(panel);
    } catch(const dice::Cancelled&) { SetText(panel,ProbabilityText,L"Calculation cancelled."); }
    catch(const std::exception& e) { SetText(panel,ProbabilityText,dice::Wide(e.what())); }
    for(int id:{FormulaA,FormulaB,ModeB,Calculate}) EnableWindow(Child(panel,id),TRUE);
    EnableWindow(Child(panel,CancelCalculation),FALSE);
    if(!panel.closing)SetFocus(Child(panel,Calculate));
    }
    if(panel.historyFuture.valid() && panel.historyFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        const auto keep=SelectedIds(panel);
        try {
            auto result=panel.historyFuture.get();
            if(result.kind==HistoryWork::Kind::Details) {
                panel.statisticsBuilds+=result.statisticsBuilt;DisplayDetails(panel,std::move(result.details));
            } else {
                panel.historyNotice=result.message.empty()?L"History metadata reloaded.":result.message;
                for(const auto& warning:result.index.warnings)panel.historyNotice+=L"\r\n"+warning;
                if(result.replaceIndex) {
                    MergeDirty(panel,result.index.entries);panel.index=std::move(result.index.entries);
                    panel.selection.index=panel.index;panel.selection.indexChanged=true;panel.cache.clear();
                    if(!panel.closing)FilterHistory(panel,false,keep);
                }
                SetText(panel,HistoryStatus,panel.historyNotice);
            }
        } catch(const dice::Cancelled&) {SetText(panel,HistoryStatus,L"Operation cancelled. Existing list and filters were kept.");}
        catch(const std::exception& e) {SetText(panel,HistoryStatus,L"Could not load history: "+dice::Wide(e.what())+L". Current list and filters were kept.");}
        if(!Working(panel) && panel.shown.empty() && !Selected(panel).empty()) {
            SetText(panel,Summary,L"Selected batch details are unavailable.");
            SetText(panel,Detail,L"Use Reload to retry, or select another batch. The original history file has been kept.");
        }
        EnableHistory(panel);LayoutPanel(panel);
        if(!panel.closing && !Working(panel))SetFocus(Child(panel,IsWindowEnabled(Child(panel,Reuse))?Reuse:Filter));
    }
    if(panel.closing && !Working(panel))DestroyWindow(panel.window);
}
void CopyFrequencyTable(Panel& panel) {
    const auto text=GetText(Child(panel,Frequencies));if(text.empty())return;
    const auto bytes=(text.size()+1)*sizeof(wchar_t);auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(!memory)throw std::runtime_error("Could not allocate clipboard memory.");
    auto data=GlobalLock(memory);if(!data){GlobalFree(memory);throw std::runtime_error("Could not access clipboard memory.");}
    memcpy(data,text.c_str(),bytes);GlobalUnlock(memory);
    if(!OpenClipboard(panel.window)){GlobalFree(memory);throw std::runtime_error("Clipboard is busy.");}
    const bool ok=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory);CloseClipboard();
    if(!ok){GlobalFree(memory);throw std::runtime_error("Could not copy frequency table.");}
}
void SetupPanel(Panel& panel) {
    WNDCLASSEXW bodyClass{sizeof(bodyClass)};bodyClass.hInstance=GetModuleHandleW(nullptr);bodyClass.lpfnWndProc=BodyProc;
    bodyClass.lpszClassName=L"DiceRollerFeatureBody";bodyClass.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
    if(!RegisterClassExW(&bodyClass) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Could not register feature body.");
    SetWindowLongPtrW(panel.window,GWL_STYLE,GetWindowLongPtrW(panel.window,GWL_STYLE)&~(WS_VSCROLL|WS_HSCROLL));
    SetWindowPos(panel.window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    panel.body=CreateWindowExW(WS_EX_CONTROLPARENT,bodyClass.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL,
        0,0,1,1,panel.window,reinterpret_cast<HMENU>(Body),bodyClass.hInstance,&panel);
    if(!panel.body)throw std::runtime_error("Could not create feature body.");
    panel.refreshing=true;
    auto add=[&](int id,const wchar_t* type,const wchar_t* text,DWORD style,DWORD ex=0) {
        const bool footer=id==Calculate || id==CancelCalculation || id==Close || id==Reuse || id==Export || id==Archive;
        auto control=CreateWindowExW(ex,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,footer?panel.window:panel.body,
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
        label(ModeLabel,L"B mode:");
        auto modes=add(ModeB,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP);
        for(const auto* mode:{L"Normal",L"Advantage",L"Disadvantage"}) SendMessageW(modes,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(mode));
        SendMessageW(modes,CB_SETCURSEL,static_cast<int>(panel.request.mode),0);
        button(Calculate,L"Calculate"); button(CancelCalculation,L"Cancel");
        EnableWindow(Child(panel,CancelCalculation),FALSE);
        add(ProbabilityText,L"EDIT",L"Change either formula or mode B, then Calculate. To change the target or pool threshold, close this window and adjust the main controls.",
            WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,WS_EX_CLIENTEDGE);
    } else {
        if(panel.entries)for(const auto& entry:*panel.entries)panel.index.push_back(dice::SummaryOf(entry));
        MergeDirty(panel,panel.index);
        label(500,L"Name:"); edit(FilterName,L"",128); label(501,L"Formula:"); edit(FilterDice,L"");
        label(502,L"UTC from:"); edit(DateFrom,L"",10); label(503,L"UTC to:"); edit(DateTo,L"",10);
        add(Unsaved,L"BUTTON",L"Pending export only",BS_AUTOCHECKBOX|WS_TABSTOP);
        add(IncludeArchived,L"BUTTON",L"Include archived batches",BS_AUTOCHECKBOX|WS_TABSTOP);
        button(Filter,L"Apply filters");button(Reload,L"Reload");
        label(504,L"Dates: YYYY-MM-DD or blank. Ctrl/Shift selects rows on this page; the first two are compared.");
        label(HistoryStatus,L"Select batches to load full details.");
        auto list=add(Batches,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SHOWSELALWAYS,WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[]={L"Created (UTC)",L"Name",L"Formula",L"Completed",L"CSV status"};
        const int widths[]={168,140,170,92,130};
        for(int i=0;i<5;++i) { LVCOLUMNW col{}; col.mask=LVCF_TEXT|LVCF_WIDTH; col.pszText=const_cast<wchar_t*>(titles[i]); col.cx=Scale(panel,widths[i]); ListView_InsertColumn(list,i,&col); }
        button(PagePrevious,L"Previous page");button(PageNext,L"Next page");label(PageLabel,L"");
        button(Reuse,L"Use parameters"); button(Export,L"Export selected");button(Archive,L"Archive selected");button(CancelCalculation,L"Cancel");
        add(Summary,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,WS_EX_CLIENTEDGE);
        label(505,L"Roll #:"); edit(RollNumber,L"1",5); button(Previous,L"Previous roll"); button(Next,L"Next roll");
        label(506,L"Full details, including dropped dice");
        auto detail=add(Detail,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL,WS_EX_CLIENTEDGE);
        SendMessageW(detail,EM_LIMITTEXT,1000000,0);
    }
    add(Chart,L"STATIC",L"",SS_OWNERDRAW);
    label(507,panel.probability?L"Probability table; exact or estimated as stated above.":L"Exact frequencies for the selected recorded batches.");
    button(CopyFrequencies,L"Copy table");
    auto frequencies=add(Frequencies,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOHSCROLL|WS_HSCROLL,WS_EX_CLIENTEDGE);
    SendMessageW(frequencies,EM_LIMITTEXT,4000000,0);
    button(Close,L"Close");
    panel.refreshing=false;Font(panel); LayoutPanel(panel);
    EnableWindow(Child(panel,CancelCalculation),FALSE);EnableWindow(Child(panel,CopyFrequencies),FALSE);
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
        case WM_GETMINMAXINFO: {
            RECT current{};GetWindowRect(window,&current);const auto work=WorkAreaFor(current);
            RECT minimum{0,0,Scale(*panel,320),Scale(*panel,280)};
            AdjustWindowRectExForDpi(&minimum,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_CONTROLPARENT,panel->dpi);
            const auto limits=reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize={(std::min)(minimum.right-minimum.left,work.right-work.left),(std::min)(minimum.bottom-minimum.top,work.bottom-work.top)};
            return 0;
        }
        case WM_SIZE: LayoutPanel(*panel);EnsurePanelFocus(*panel);return 0;
        case WM_DPICHANGED: {
            const UINT oldDpi=panel->dpi;
            panel->dpi=HIWORD(wp); Font(*panel);
            if(!panel->probability) for(int i=0;i<5;++i)
                ListView_SetColumnWidth(Child(*panel,Batches),i,MulDiv(ListView_GetColumnWidth(Child(*panel,Batches),i),static_cast<int>(panel->dpi),static_cast<int>(oldDpi)));
            auto r=*reinterpret_cast<RECT*>(lp); r=FitToWorkArea(r,WorkAreaFor(r));
            SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
            LayoutPanel(*panel);EnsurePanelFocus(*panel);return 0;
        }
        case WM_MOUSEWHEEL:return SendMessageW(panel->body,message,wp,lp);
        case WM_DRAWITEM: { auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp); if(draw->CtlID==Chart) {DrawChart(*panel,draw->hDC,draw->rcItem);return TRUE;} break; }
        case WM_NOTIFY:
            if(reinterpret_cast<NMHDR*>(lp)->idFrom==Batches && reinterpret_cast<NMHDR*>(lp)->code==LVN_ITEMCHANGED && !panel->refreshing)panel->detailsPending=true;
            return 0;
        case WM_TIMER: Tick(*panel);return 0;
        case WM_COMMAND: {
            const int id=LOWORD(wp);
            if(panel->probability && (((id==FormulaA || id==FormulaB) && HIWORD(wp)==EN_CHANGE) ||
                (id==ModeB && HIWORD(wp)==CBN_SELCHANGE))) {MarkStale(*panel);return 0;}
            if(id==RollNumber && HIWORD(wp)==EN_KILLFOCUS) {
                if(!panel->refreshing)CommitRollNumber(*panel);return 0;
            }
            if(lp && HIWORD(wp)!=BN_CLICKED) return 0;
            switch(id) {
            case Filter:FilterHistory(*panel);break;
            case IncludeArchived:case Reload:ReloadHistory(*panel,false);break;
            case Archive:ReloadHistory(*panel,true);break;
            case PagePrevious:if(panel->page){--panel->page;FilterHistory(*panel,false);}break;
            case PageNext:if((panel->page+1)*PageSize<panel->matches.size()){++panel->page;FilterHistory(*panel,false);}break;
            case Previous: if(panel->roll) --panel->roll;RenderRoll(*panel);break;
            case Next:++panel->roll;RenderRoll(*panel);break;
            case Reuse:case Export:
                if(Working(*panel))break;
                panel->selection.indices=Selected(*panel);
                if(panel->selection.indices.empty() || (id==Reuse && panel->selection.indices.size()!=1)) break;
                panel->selection.selected.clear();
                for(auto i:panel->selection.indices)panel->selection.selected.push_back(panel->index[i]);
                if(id==Export && panel->exportPreflight)panel->exportPreflight();
                if(id==Reuse)panel->selection.request=panel->selection.selected.front().request;
                panel->selection.action=id==Reuse?HistorySelection::Action::Reuse:HistorySelection::Action::Export;
                DestroyWindow(window);break;
            case Calculate:CalculateProbability(*panel);break;
            case CopyFrequencies:CopyFrequencyTable(*panel);break;
            case CancelCalculation:panel->cancel=true;break;
            case IDOK:
                if(panel->probability)CalculateProbability(*panel);
                else if(GetFocus()==Child(*panel,RollNumber))CommitRollNumber(*panel);
                else if(GetFocus()==Child(*panel,FilterName) || GetFocus()==Child(*panel,FilterDice) ||
                    GetFocus()==Child(*panel,DateFrom) || GetFocus()==Child(*panel,DateTo))FilterHistory(*panel);
                break;
            case IDCANCEL:case Close:SendMessageW(window,WM_CLOSE,0,0);break;
            }
            return 0;
        }
        case WM_CLOSE:
            if(Working(*panel)) { panel->closing=true;panel->cancel=true;EnableWindow(Child(*panel,CancelCalculation),FALSE); }
            else DestroyWindow(window);return 0;
        case WM_DESTROY:KillTimer(window,1);return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
        }
    } catch(const std::exception& e) {
        if(message==WM_CREATE)return -1;
        SetText(*panel,panel->probability?ProbabilityText:HistoryStatus,dice::Wide(e.what()));LayoutPanel(*panel);
    }
    return DefWindowProcW(window,message,wp,lp);
}
void RunPanel(Panel& panel) {
    INITCOMMONCONTROLSEX init{sizeof(init),ICC_LISTVIEW_CLASSES};
    if(!InitCommonControlsEx(&init))throw std::runtime_error("Could not initialize feature controls.");
    WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=PanelProc;
    wc.lpszClassName=L"DiceRollerFeatures";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
    if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Could not register the feature window.");
    RECT owner{};GetWindowRect(panel.owner,&owner);
    const int dpi=static_cast<int>(GetDpiForWindow(panel.owner));
    RECT rect{owner.left+20,owner.top+20,owner.left+20+MulDiv(900,dpi,96),owner.top+20+MulDiv(panel.probability?780:960,dpi,96)};
    rect=FitToWorkArea(rect,WorkAreaFor(rect));
    auto window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,panel.probability?L"Probability and comparison":L"History, recovery and statistics",
        WS_OVERLAPPEDWINDOW,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,panel.owner,nullptr,wc.hInstance,&panel);
    if(!window) throw std::runtime_error("Could not open the feature window.");
    EnableWindow(panel.owner,FALSE);ShowWindow(window,SW_SHOW);SetFocus(Child(panel,Working(panel)?CancelCalculation:panel.probability?FormulaA:FilterName));
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
std::wstring BatchSummary(const dice::HistoryEntry& entry) { return DescribeBatch(entry,dice::Summarize(entry.batch)); }
HistorySelection ShowHistory(HWND owner,const std::vector<dice::HistoryEntry>& entries) {
    Panel panel;panel.owner=owner;panel.entries=&entries;RunPanel(panel);return panel.selection;
}
HistorySelection ShowHistory(HWND owner,const std::filesystem::path& root,std::vector<dice::HistorySummary> index,const std::vector<dice::HistoryEntry>& dirty,const std::function<void()>& exportPreflight,const std::vector<dice::HistorySummary>& pendingExportMarks) {
    Panel panel;panel.owner=owner;panel.root=root;panel.index=std::move(index);panel.dirty=&dirty;panel.exportPreflight=exportPreflight;panel.pendingExportMarks=pendingExportMarks;RunPanel(panel);return panel.selection;
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
