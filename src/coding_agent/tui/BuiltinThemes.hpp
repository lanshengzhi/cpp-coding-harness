#pragma once

#include <string_view>

namespace cch::coding_agent::tui::detail {

// Dark palette transcribed from pi v0.99.2 at commit
// 005af57d88ee23b33778f343a9595b32e67ff788. The light palette remains pinned
// to the earlier pi theme baseline recorded in tests/fixtures/themes/README.md.
inline constexpr std::string_view kBuiltinDarkThemeJson = R"CCH_THEME(
{
	"$schema": "https://raw.githubusercontent.com/earendil-works/pi/main/packages/coding-agent/src/modes/interactive/theme/theme-schema.json",
	"name": "dark",
	"vars": {
		"text": "#dee0e1",
		"muted": "#9da5a9",
		"violet": "#a798d7",
		"blue": "#69add0",
		"green": "#68b78d",
		"red": "#ea7f81",
		"yellow": "#cd9a22",
		"blueBg": "#213b49"
	},
	"colors": {
		"accent": "violet",
		"border": "#5fa8cc",
		"borderAccent": "#a08ed5",
		"borderMuted": "#768186",
		"success": "green",
		"error": "red",
		"warning": "yellow",
		"muted": "muted",
		"dim": "#7e888e",
		"text": "text",
		"thinkingText": "#96a0a4",
		"selectedBg": "blueBg",
		"scrollbarThumb": "#97a0a5",
		"userMessageBg": "blueBg",
		"userMessageText": "text",
		"customMessageBg": "#3a3055",
		"customMessageText": "muted",
		"customMessageLabel": "violet",
		"toolPendingBg": "#34383a",
		"toolSuccessBg": "#254131",
		"toolErrorBg": "#5b282a",
		"toolTitle": "text",
		"toolOutput": "muted",
		"mdHeading": "yellow",
		"mdLink": "blue",
		"mdLinkUrl": "muted",
		"mdCode": "violet",
		"mdCodeBlock": "green",
		"mdCodeBlockBorder": "muted",
		"mdQuote": "muted",
		"mdQuoteBorder": "muted",
		"mdHr": "muted",
		"mdListBullet": "violet",
		"toolDiffAdded": "green",
		"toolDiffRemoved": "red",
		"toolDiffContext": "muted",
		"syntaxComment": "muted",
		"syntaxKeyword": "blue",
		"syntaxFunction": "yellow",
		"syntaxVariable": "#5db3ba",
		"syntaxString": "#de8d5a",
		"syntaxNumber": "green",
		"syntaxType": "violet",
		"syntaxOperator": "muted",
		"syntaxPunctuation": "muted",
		"thinkingOff": "#6c767b",
		"thinkingMinimal": "#68808d",
		"thinkingLow": "#5489a4",
		"thinkingMedium": "#6185cc",
		"thinkingHigh": "#9776e5",
		"thinkingXhigh": "#de54c1",
		"thinkingMax": "#fe5462",
		"bashMode": "#5eb286"
	},
	"export": {
		"pageBg": "#21252c",
		"cardBg": "#282c34",
		"infoBg": "#4e2f1b"
	}
}
)CCH_THEME";

inline constexpr std::string_view kBuiltinLightThemeJson = R"CCH_THEME(
{
	"$schema": "https://raw.githubusercontent.com/earendil-works/pi/main/packages/coding-agent/src/modes/interactive/theme/theme-schema.json",
	"name": "light",
	"vars": {
		"teal": "#5a8080",
		"blue": "#547da7",
		"green": "#588458",
		"red": "#aa5555",
		"yellow": "#9a7326",
		"text": "#1f2328",
		"mediumGray": "#6c6c6c",
		"dimGray": "#767676",
		"lightGray": "#b0b0b0",
		"selectedBg": "#d0d0e0",
		"userMsgBg": "#e8e8e8",
		"toolPendingBg": "#e8e8f0",
		"toolSuccessBg": "#e8f0e8",
		"toolErrorBg": "#f0e8e8",
		"customMsgBg": "#ede7f6"
	},
	"colors": {
		"accent": "teal",
		"border": "blue",
		"borderAccent": "teal",
		"borderMuted": "lightGray",
		"success": "green",
		"error": "red",
		"warning": "yellow",
		"muted": "mediumGray",
		"dim": "dimGray",
		"text": "text",
		"thinkingText": "mediumGray",

		"selectedBg": "selectedBg",
		"scrollbarThumb": "selectedBg",
		"userMessageBg": "userMsgBg",
		"userMessageText": "text",
		"customMessageBg": "customMsgBg",
		"customMessageText": "text",
		"customMessageLabel": "#7e57c2",
		"toolPendingBg": "toolPendingBg",
		"toolSuccessBg": "toolSuccessBg",
		"toolErrorBg": "toolErrorBg",
		"toolTitle": "text",
		"toolOutput": "mediumGray",

		"mdHeading": "yellow",
		"mdLink": "blue",
		"mdLinkUrl": "dimGray",
		"mdCode": "teal",
		"mdCodeBlock": "green",
		"mdCodeBlockBorder": "mediumGray",
		"mdQuote": "mediumGray",
		"mdQuoteBorder": "mediumGray",
		"mdHr": "mediumGray",
		"mdListBullet": "green",

		"toolDiffAdded": "green",
		"toolDiffRemoved": "red",
		"toolDiffContext": "mediumGray",

		"syntaxComment": "#008000",
		"syntaxKeyword": "#0000FF",
		"syntaxFunction": "#795E26",
		"syntaxVariable": "#001080",
		"syntaxString": "#A31515",
		"syntaxNumber": "#098658",
		"syntaxType": "#267F99",
		"syntaxOperator": "#000000",
		"syntaxPunctuation": "#000000",

		"thinkingOff": "lightGray",
		"thinkingMinimal": "#767676",
		"thinkingLow": "blue",
		"thinkingMedium": "teal",
		"thinkingHigh": "#875f87",
		"thinkingXhigh": "#8b008b",
		"thinkingMax": "#af005f",

		"bashMode": "green"
	},
	"export": {
		"pageBg": "#f8f8f8",
		"cardBg": "#ffffff",
		"infoBg": "#fffae6"
	}
}
)CCH_THEME";

} // namespace cch::coding_agent::tui::detail
