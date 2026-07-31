using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.Input;

namespace Aegisub.LocaleViewer;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        var filterBox = this.FindControl<TextBox>("EntryFilterBox");
        if (filterBox is not null)
        {
            filterBox.AttachedToVisualTree += (_, _) =>
            {
                if (DataContext is MainWindowViewModel vm)
                    vm.PropertyChanged += (_, e) =>
                    {
                        if (e.PropertyName == nameof(MainWindowViewModel.SelectedLanguage) && vm.SelectedLanguage is not null)
                            Dispatcher.UIThread.Post(() => filterBox.Focus(), DispatcherPriority.Loaded);
                    };
            };
        }

        if (DataContext is MainWindowViewModel viewModel)
        {
            KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.S, KeyModifiers.Control), Command = viewModel.SaveCommand });
            KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.F5), Command = viewModel.RefreshCommand });
        }

        KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.F, KeyModifiers.Control), Command = new RelayCommand(() => this.FindControl<TextBox>("EntryFilterBox")?.Focus()) });
        KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.D1, KeyModifiers.Control), Command = new RelayCommand(() => { var cb = this.FindControl<ComboBox>("StateFilter"); if (cb is not null) cb.SelectedIndex = 0; }) });
        KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.D2, KeyModifiers.Control), Command = new RelayCommand(() => { var cb = this.FindControl<ComboBox>("StateFilter"); if (cb is not null) cb.SelectedIndex = 1; }) });
        KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.D3, KeyModifiers.Control), Command = new RelayCommand(() => { var cb = this.FindControl<ComboBox>("StateFilter"); if (cb is not null) cb.SelectedIndex = 2; }) });
        KeyBindings.Add(new KeyBinding { Gesture = new KeyGesture(Key.D4, KeyModifiers.Control), Command = new RelayCommand(() => { var cb = this.FindControl<ComboBox>("StateFilter"); if (cb is not null) cb.SelectedIndex = 3; }) });
    }
}
