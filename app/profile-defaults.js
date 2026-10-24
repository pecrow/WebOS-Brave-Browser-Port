'use strict';

module.exports = function (state, preferences) {
    var homepage = 'https://github.com/pecrow/WebOS-Brave-Browser-Port/';
    preferences.homepage = homepage;
    preferences.homepage_is_newtabpage = false;
    preferences.browser = preferences.browser || {};
    preferences.browser.show_home_button = true;
    preferences.session = preferences.session || {};
    preferences.session.restore_on_startup = 4;
    preferences.session.startup_urls = [homepage];
    state.performance_tuning = state.performance_tuning || {};
    state.performance_tuning.high_efficiency_mode =
        state.performance_tuning.high_efficiency_mode || {};
    state.performance_tuning.high_efficiency_mode.state = 2;
    state.performance_tuning.high_efficiency_mode.aggressiveness = 1;
    preferences.profile = preferences.profile || {};
    preferences.profile.content_settings = preferences.profile.content_settings || {};
    var settings = preferences.profile.content_settings;
    settings.exceptions = settings.exceptions || {};
    settings.exceptions.autoplay = settings.exceptions.autoplay || {};
    ['[*.]youtube.com,*', 'https://www.youtube.com:443,*'].forEach(function (pattern) {
        var rule = settings.exceptions.autoplay[pattern] || {};
        rule.setting = 2;
        settings.exceptions.autoplay[pattern] = rule;
    });
};
