#include "pch.h"
#include "MainWindow.xaml.h"
#include "Integration/ToolCatalog.h"
#include "Integration/SearchCatalog.h"
#include "Integration/I18n.h"
#include <algorithm>
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
namespace winrt::AeluraTool::implementation {
using aelura::i18n::tr;
void MainWindow::SetupSearch() {
    Search().UpdateTextOnSelect(false);
    Search().TextChanged([this](auto const&, auto const&) { if (!restoringNavigation_) UpdateSearch(); });
    Search().QuerySubmitted([this](AutoSuggestBox const&, AutoSuggestBoxQuerySubmittedEventArgs const& args) {
        if (args.ChosenSuggestion()) OpenSearchResult(unbox_value<hstring>(args.ChosenSuggestion()));
        else if (!searchTargets_.empty()) OpenSearchResult(hstring(searchTargets_.front().label));
    });
}
void MainWindow::UpdateSearch() {
    searchTargets_.clear(); auto suggestions = single_threaded_observable_vector<IInspectable>();
    const std::wstring query(Search().Text());
    if (query.find_first_not_of(L" \t\r\n") != std::wstring::npos) {
        auto add = [&](std::wstring title, std::wstring path, std::wstring keywords, std::wstring id, int page, int category) {
            if (!aelura::catalog::matches(title + L" " + path + L" " + keywords, query)) return;
            auto label = title + L"  ·  " + path;
            searchTargets_.push_back({label, id, page, category}); suggestions.Append(box_value(label));
        };
        for (auto const& tool : aelura::catalog::tools)
            add(tr(tool.title), aelura::i18n::category(tool.category), std::wstring(tool.keywords) + L" " + tr(tool.description), tool.id, -1, tool.category);
        for (auto const& entry : aelura::search::settings)
            add(tr(entry.title), tr(entry.page == 2 ? "nav.settings" : entry.page == 1 ? "nav.keys" : "nav.categories"), entry.keywords, entry.id, entry.page, entry.category);
        if (searchTargets_.empty()) suggestions.Append(box_value(tr("search.no_global_results")));
    }
    Search().ItemsSource(suggestions);
}
void MainWindow::OpenSearchResult(hstring const& label) {
    auto found = std::find_if(searchTargets_.begin(), searchTargets_.end(), [&](auto const& target) { return target.label == label.c_str(); });
    if (found == searchTargets_.end()) return;
    const auto target = *found; Search().IsSuggestionListOpen(false); FinishStartup();
    if (target.page == -1) { ActivateTool(target.id); return; }
    Page(target.page, target.category ? target.category : category_);
    FrameworkElement element{nullptr};
    if (target.id == L"appearance") element = ThemeChoice();
    else if (target.id == L"language") element = LanguageChoice();
    else if (target.id == L"motion") element = StartupMotionSwitch();
    else if (target.id == L"close") element = CloseActionChoice();
    else if (target.id == L"autostart") element = AutoStartSwitch();
    else if (target.id == L"licenses") element = LicensesButton();
    if (element) DispatcherQueue().TryEnqueue([element] {
        element.StartBringIntoView();
        if (auto control = element.try_as<Control>()) control.Focus(FocusState::Programmatic);
    });
}
}
