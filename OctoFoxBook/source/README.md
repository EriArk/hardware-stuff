# Исходники OctoFox Book

Основные редактируемые модели — [STEP/parts](../STEP/parts/). Они содержат всю механику и крупный декоративный рельеф. Готовые фактурные STL лежат отдельно в [STL/print-set](../STL/print-set/).

Скрипты используют Python 3.12, CadQuery/OpenCascade, NumPy, SciPy и VTK. Проверенная версия CadQuery — 2.8.0. Запускать из папки `OctoFox Book`:

```sh
python -m venv .venv
# Активируйте окружение командой для своей ОС.
python -m pip install -r source/requirements.txt
```

## Что можно перестроить

```sh
python source/build_front_relief.py
python source/build_sleep.py
python source/scale_texture.py
python source/build_assembly.py
```

- `build_front_relief.py` берёт `base/front_before_relief.brep` и параметры из `docs/relief_parameters.json`, строит три тентакля с переходом на боковины. Результат — основа передней панели v65 в `build/front-relief/`.
- `build_sleep.py` использует этот результат, исправляет окно Sleep и строит колпачок v66. Выходные STEP/BREP и **гладкие** STL находятся в `build/sleep/`.
- `scale_texture.py` содержит функцию микрочешуи и строит три пробные пластинки: шаг/высота 0,6/0,06; 0,8/0,10; 1,0/0,14 мм. Выход — `build/texture_samples/`.
- `build_assembly.py` собирает семь текущих STEP из `parts.json`; задняя крышка смещается на +18 мм по Z. Выход — `build/AbyssBook_assembly.step`, если другой путь не задан через `--output`.

Скрипты не перезаписывают готовый печатный набор. Генератор пробников не воспроизводит целиком финальную плотную сетку корпуса: готовые STL включают отдельно выполненные уплотнение сетки, маски внутренних поверхностей и локальные исправления, описанные в `docs/verification`. Их проверенные результаты сохранены в Git LFS. Для изменения формы всей читалки используйте STEP; для печати текущей версии — готовые STL.

`reference/` содержит пользовательский логотип и присланные чертежи переключателей. Служебные файлы загрузки на сервер, доступы и локальные временные данные сюда не входят.
