# config/ の設定ファイルを実行ファイルの隣にコピーする
#   cmake -DSOURCE_DIR=... -DDESTINATION_DIR=... -P sync_config.cmake
#
# 設定画面で保存した設定をビルドで上書きしないように、
# コピー先が無いか、コピー元のほうが新しいファイルだけをコピーする
# (コピー元を書き換えれば、そちらが新しくなるので次のビルドで反映される)

file(GLOB_RECURSE files RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")

foreach(file IN LISTS files)
    set(source "${SOURCE_DIR}/${file}")
    set(destination "${DESTINATION_DIR}/${file}")

    # 更新日時が同じときも IS_NEWER_THAN は真になる
    # configure_file は中身が同じなら書かないので、同じ内容を書き直すことはない
    # (file(COPY) は更新日時を秒単位で比べて、同じ秒の変更を見落とすので使わない)
    if(NOT EXISTS "${destination}" OR "${source}" IS_NEWER_THAN "${destination}")
        configure_file("${source}" "${destination}" COPYONLY)
    endif()
endforeach()
