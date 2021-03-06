import { HttpClientTestingModule } from '@angular/common/http/testing';
import { async, ComponentFixture, TestBed } from '@angular/core/testing';
import { BrowserAnimationsModule } from '@angular/platform-browser/animations';
import { RouterTestingModule } from '@angular/router/testing';

import * as _ from 'lodash';
import { BsDropdownModule } from 'ngx-bootstrap/dropdown';
import { TabsModule } from 'ngx-bootstrap/tabs';
import { ToastrModule } from 'ngx-toastr';
import { of } from 'rxjs';

import { configureTestBed, i18nProviders } from '../../../../testing/unit-test-helper';
import { CoreModule } from '../../../core/core.module';
import { HostService } from '../../../shared/api/host.service';
import { ActionLabels } from '../../../shared/constants/app.constants';
import { CdTableAction } from '../../../shared/models/cd-table-action';
import { Permissions } from '../../../shared/models/permissions';
import { AuthStorageService } from '../../../shared/services/auth-storage.service';
import { SharedModule } from '../../../shared/shared.module';
import { CephModule } from '../../ceph.module';
import { CephSharedModule } from '../../shared/ceph-shared.module';
import { HostsComponent } from './hosts.component';

describe('HostsComponent', () => {
  let component: HostsComponent;
  let fixture: ComponentFixture<HostsComponent>;
  let hostListSpy: jasmine.Spy;

  const fakeAuthStorageService = {
    getPermissions: () => {
      return new Permissions({ hosts: ['read', 'update', 'create', 'delete'] });
    }
  };

  configureTestBed({
    imports: [
      BrowserAnimationsModule,
      CephSharedModule,
      SharedModule,
      HttpClientTestingModule,
      TabsModule.forRoot(),
      BsDropdownModule.forRoot(),
      RouterTestingModule,
      ToastrModule.forRoot(),
      CephModule,
      CoreModule
    ],
    providers: [{ provide: AuthStorageService, useValue: fakeAuthStorageService }, i18nProviders],
    declarations: []
  });

  beforeEach(() => {
    fixture = TestBed.createComponent(HostsComponent);
    component = fixture.componentInstance;
    hostListSpy = spyOn(TestBed.get(HostService), 'list');
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('should render hosts list even with not permission mapped services', async(() => {
    const hostname = 'ceph.dev';
    const payload = [
      {
        services: [
          {
            type: 'osd',
            id: '0'
          },
          {
            type: 'rgw',
            id: 'rgw'
          },
          {
            type: 'notPermissionMappedService',
            id: '1'
          }
        ],
        hostname: hostname,
        ceph_version: 'ceph version Development',
        labels: ['foo', 'bar']
      }
    ];

    hostListSpy.and.callFake(() => of(payload));

    fixture.whenStable().then(() => {
      fixture.detectChanges();

      const spans = fixture.debugElement.nativeElement.querySelectorAll(
        '.datatable-body-cell-label span'
      );
      expect(spans[0].textContent).toBe(hostname);
    });
  }));

  describe('test edit button', () => {
    let tableAction: CdTableAction;

    beforeEach(() => {
      tableAction = _.find(component.tableActions, { name: ActionLabels.EDIT });
    });

    it('should disable button and return message (not managed by Orchestrator)', () => {
      component.selection.add({
        sources: {
          ceph: true,
          orchestrator: false
        }
      });
      expect(tableAction.disable(component.selection)).toBeTruthy();
      expect(component.getEditDisableDesc(component.selection)).toBe(
        'Host editing is disabled because the selected host is not managed by Orchestrator.'
      );
    });

    it('should disable button and return undefined (no selection)', () => {
      expect(tableAction.disable(component.selection)).toBeTruthy();
      expect(component.getEditDisableDesc(component.selection)).toBeUndefined();
    });

    it('should enable button and return undefined (managed by Orchestrator)', () => {
      component.selection.add({
        sources: {
          ceph: false,
          orchestrator: true
        }
      });
      expect(tableAction.disable(component.selection)).toBeFalsy();
      expect(component.getEditDisableDesc(component.selection)).toBeUndefined();
    });
  });

  describe('getDeleteDisableDesc', () => {
    it('should return message (not managed by Orchestrator)', () => {
      component.selection.add({
        sources: {
          ceph: false,
          orchestrator: true
        }
      });
      component.selection.add({
        sources: {
          ceph: true,
          orchestrator: false
        }
      });
      expect(component.getDeleteDisableDesc(component.selection)).toBe(
        'Host deletion is disabled because a selected host is not managed by Orchestrator.'
      );
    });

    it('should return undefined (no selection)', () => {
      expect(component.getDeleteDisableDesc(component.selection)).toBeUndefined();
    });

    it('should return undefined (managed by Orchestrator)', () => {
      component.selection.add({
        sources: {
          ceph: false,
          orchestrator: true
        }
      });
      component.selection.add({
        sources: {
          ceph: false,
          orchestrator: true
        }
      });
      expect(component.getDeleteDisableDesc(component.selection)).toBeUndefined();
    });
  });
});
