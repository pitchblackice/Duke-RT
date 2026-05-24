
#include "settingspage.h"
#include "launcherwindow.h"
#include "gstrings.h"
#include "i_interface.h"
#include "v_video.h"
#include <zwidget/core/resourcedata.h>
#include <zwidget/widgets/listview/listview.h>
#include <zwidget/widgets/textlabel/textlabel.h>
#include <zwidget/widgets/checkboxlabel/checkboxlabel.h>

EXTERN_CVAR(String, language)
EXTERN_CVAR(Bool, queryiwad);

SettingsPage::SettingsPage(LauncherWindow* launcher, int* autoloadflags) : Widget(nullptr), Launcher(launcher), AutoloadFlags(autoloadflags)
{
	LangLabel = new TextLabel(this);
	GeneralLabel = new TextLabel(this);
	//ExtrasLabel = new TextLabel(this);
	FullscreenCheckbox = new CheckboxLabel(this);
	DisableAutoloadCheckbox = new CheckboxLabel(this);
	DontAskAgainCheckbox = new CheckboxLabel(this);
	/*
	LightsCheckbox = new CheckboxLabel(this);
	BrightmapsCheckbox = new CheckboxLabel(this);
	WidescreenCheckbox = new CheckboxLabel(this);
	*/

	FullscreenCheckbox->SetChecked(vid_fullscreen);
	DontAskAgainCheckbox->SetChecked(!queryiwad);

	int flags = *autoloadflags;
	DisableAutoloadCheckbox->SetChecked(flags & 1);
	/*
	LightsCheckbox->SetChecked(flags & 2);
	BrightmapsCheckbox->SetChecked(flags & 4);
	WidescreenCheckbox->SetChecked(flags & 8);
	*/

	LangList = new ListView(this);

	try
	{
		auto data = LoadWidgetData("menudef.txt");
		FScanner sc;
		sc.OpenMem("menudef.txt", data);
		while (sc.GetString())
		{
			if (sc.Compare("OptionString"))
			{
				sc.MustGetString();
				if (sc.Compare("LanguageOptions"))
				{
					sc.MustGetStringName("{");
					while (!sc.CheckString("}"))
					{
						sc.MustGetString();
						FString iso = sc.String;
						sc.MustGetStringName(",");
						sc.MustGetString();
						if(iso.CompareNoCase("auto"))
							languages.push_back(std::make_pair(iso, FString(sc.String)));
					}
				}
			}
		}
	}
	catch (const std::exception&)
	{
		hideLanguage = true;
	}
	int i = 0;
	for (auto& l : languages)
	{
		LangList->AddItem(l.second.GetChars());
		if (!l.first.CompareNoCase(::language))
			LangList->SetSelectedItem(i);
		i++;
	}

	LangList->OnChanged = [=](int i) { OnLanguageChanged(i); };
}

void SettingsPage::Save()
{
	vid_fullscreen = FullscreenCheckbox->GetChecked();
	queryiwad = !DontAskAgainCheckbox->GetChecked();

	int flags = 0;
	if (DisableAutoloadCheckbox->GetChecked()) flags |= 1;
	/*
	if (LightsCheckbox->GetChecked()) flags |= 2;
	if (BrightmapsCheckbox->GetChecked()) flags |= 4;
	if (WidescreenCheckbox->GetChecked()) flags |= 8;
	*/
	*AutoloadFlags = flags;

}

void SettingsPage::UpdateLanguage()
{
	LangLabel->SetText(GStrings.GetString("OPTMNU_LANGUAGE"));
	GeneralLabel->SetText(GStrings.GetString("PICKER_GENERAL"));
//	ExtrasLabel->SetText(GStrings.GetString("PICKER_EXTRA"));
	FullscreenCheckbox->SetText(GStrings.GetString("PICKER_FULLSCREEN"));
	DisableAutoloadCheckbox->SetText(GStrings.GetString("PICKER_NOAUTOLOAD"));
	DontAskAgainCheckbox->SetText(GStrings.GetString("PICKER_DONTASK"));
	/*
	LightsCheckbox->SetText(GStrings.GetString("PICKER_LIGHTS"));
	BrightmapsCheckbox->SetText(GStrings.GetString("PICKER_BRIGHTMAPS"));
	WidescreenCheckbox->SetText(GStrings.GetString("PICKER_WIDESCREEN"));
	*/

}

void SettingsPage::OnLanguageChanged(int i)
{
	::language = languages[i].first.GetChars();
	GStrings.UpdateLanguage(::language); // CVAR callbacks are not active yet.
	UpdateLanguage();
	Update();
	Launcher->UpdateLanguage();
}

void SettingsPage::OnGeometryChanged()
{
	double y = 0.0;
	double w = GetWidth();
	double h = GetHeight();

	GeneralLabel->SetFrameGeometry(0.0, y, 190.0, GeneralLabel->GetPreferredHeight());
	//ExtrasLabel->SetFrameGeometry(w - panelWidth, y, panelWidth, ExtrasLabel->GetPreferredHeight());
	y += GeneralLabel->GetPreferredHeight();

	FullscreenCheckbox->SetFrameGeometry(0.0, y, 190.0, FullscreenCheckbox->GetPreferredHeight());
	//LightsCheckbox->SetFrameGeometry(w - panelWidth, y, panelWidth, LightsCheckbox->GetPreferredHeight());
	y += FullscreenCheckbox->GetPreferredHeight();

	DisableAutoloadCheckbox->SetFrameGeometry(0.0, y, 190.0, DisableAutoloadCheckbox->GetPreferredHeight());
	//BrightmapsCheckbox->SetFrameGeometry(w - panelWidth, y, panelWidth, BrightmapsCheckbox->GetPreferredHeight());
	y += DisableAutoloadCheckbox->GetPreferredHeight();

	DontAskAgainCheckbox->SetFrameGeometry(0.0, y, 190.0, DontAskAgainCheckbox->GetPreferredHeight());
	//WidescreenCheckbox->SetFrameGeometry(w - panelWidth, y, panelWidth, WidescreenCheckbox->GetPreferredHeight());
	y += DontAskAgainCheckbox->GetPreferredHeight();

	if (!hideLanguage)
	{
		LangLabel->SetFrameGeometry(0.0, y, w, LangLabel->GetPreferredHeight());
		y += LangLabel->GetPreferredHeight();
		LangList->SetFrameGeometry(0.0, y, w, std::max(h - y, 0.0));
	}
}
