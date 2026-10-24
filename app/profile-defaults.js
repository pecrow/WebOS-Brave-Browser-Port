'use strict';

module.exports = function (state, preferences) {
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
