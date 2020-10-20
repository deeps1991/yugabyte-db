import React, { FC } from 'react';

type I18nProps = Record<string, unknown>;

// TODO: add real i18n support
export const I18n: FC<I18nProps> = (props) => <span {...props} />;
