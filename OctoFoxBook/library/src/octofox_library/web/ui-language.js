'use strict';
for (const select of document.querySelectorAll('.ui-language')) {
  select.value = document.documentElement.lang;
  select.addEventListener('change', () => {
    if (!['en', 'ru'].includes(select.value)) return;
    document.cookie = `octofox_lang=${select.value}; Path=/; Max-Age=31536000; SameSite=Lax${location.protocol === 'https:' ? '; Secure' : ''}`;
    location.reload();
  });
}
